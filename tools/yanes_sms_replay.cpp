// Replays SN76489 writes captured from a real SMS ROM run by the patched Genesis
// Plus GX core, and renders them through the plug-in's own PSG primitives, so the
// result can be scored against the real-game audio the same run produced.
//
// The SN76489 is three square-tone channels and one LFSR noise channel, each with
// a 4-bit attenuator, driven by a latch/data byte protocol. The replay rebuilds
// the register state from the raw byte stream on the log's monotonic master clock
// (Genesis Plus GX clocks the PSG so master/480 equals the real SMS tone
// frequency), integrates each channel's piecewise-constant output exactly over an
// oversampled window, and decimates through the shared windowed-sinc filter so
// the comparison is not coloured by aliased step edges.
//
// Provenance: the noise generator is the plug-in's own yanes::sega_psg_lfsr_clock
// from src/dsp.hpp, and the squares are its two-level output; the control layer
// (the latch protocol, counters and attenuator table) was written from the
// SN76489, which is also the oracle, so that half is a gate on the capture and
// timeline rather than independent evidence.
#include "../src/dsp.hpp"
#include "replay_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kMaster = 53693175.0;          // Genesis Plus GX master clock (NTSC)
constexpr double kFrameMcycles = 3420.0 * 262.0;  // M-cycles per NTSC frame
constexpr double kPsgRatio = 15.0 * 16.0;        // internal clock = master / 240
constexpr int kRate = 44100;
constexpr int kOversample = 8;
constexpr int kSubRate = kRate * kOversample;
constexpr double kSubWindow = kMaster / kSubRate;  // master cycles per oversampled sample

struct Write { int64_t clock; uint8_t data; };

bool load(const std::string& path, std::vector<Write>& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    long long frame = 0, clock = 0; unsigned data = 0;
    if (!(s >> frame >> clock >> std::hex >> data)) continue;
    out.push_back({static_cast<int64_t>(frame * kFrameMcycles + clock), static_cast<uint8_t>(data & 0xff)});
  }
  std::sort(out.begin(), out.end(), [](const Write& a, const Write& b) { return a.clock < b.clock; });
  return !out.empty();
}

// SN76489 attenuator: 2 dB per step, 15 is silence.
double gain(int atten) { return atten >= 15 ? 0.0 : std::pow(10.0, -0.1 * atten); }

// Mean of a +/-1 square of half-period H over a window of W master cycles,
// advancing `phase` (kept in [0, 2H)).
double integrate_square(double& phase, double H, double W) {
  const double period = 2.0 * H;
  if (phase >= period) phase = std::fmod(phase, period);
  double area = 0.0, remaining = W;
  while (remaining > 1e-9) {
    const double level = phase < H ? 1.0 : -1.0;
    const double edge = phase < H ? H : period;
    const double seg = std::min(remaining, edge - phase);
    area += level * seg;
    phase += seg;
    if (phase >= period) phase -= period;
    remaining -= seg;
  }
  return area / W;
}

// Mean of the noise channel over W cycles: its LFSR shifts every Rn cycles, and
// the current bit sets the +/-1 level between shifts.
double integrate_noise(double& phase, double Rn, double W, uint32_t& lfsr, bool white) {
  double area = 0.0, remaining = W;
  double level = (lfsr & 1U) ? 1.0 : -1.0;
  while (remaining > 1e-9) {
    const double seg = std::min(remaining, Rn - phase);
    area += level * seg;
    phase += seg;
    remaining -= seg;
    if (phase >= Rn - 1e-9) {
      phase -= Rn;
      lfsr = yanes::sega_psg_lfsr_clock(lfsr, white);
      level = (lfsr & 1U) ? 1.0 : -1.0;
    }
  }
  return area / W;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: yanes-sms-replay log outdir [seconds]\n"); return 2; }
  std::vector<Write> writes;
  if (!load(argv[1], writes)) { std::fprintf(stderr, "no SN76489 writes in %s\n", argv[1]); return 1; }
  const std::string dir = argv[2];
  const double seconds = argc >= 4 ? std::atof(argv[3]) : 0.0;
  const int64_t total_cycles = seconds > 0 ? static_cast<int64_t>(seconds * kMaster)
                                           : writes.back().clock + static_cast<int64_t>(kMaster * 0.2);
  const int out_frames = static_cast<int>(static_cast<double>(total_cycles) / kMaster * kRate);

  // Power-on state: every channel attenuated to silence, as the chip resets.
  int regs[8] = {0, 0x0f, 0, 0x0f, 0, 0x0f, 0, 0x0f};
  int latch = 0;
  double tone_phase[3] = {0, 0, 0}, noise_phase = 0;
  uint32_t lfsr = 0x8000;
  size_t wi = 0;
  int64_t clock = 0;

  yanes::replay::Decimator<kOversample> decim;
  std::vector<int16_t> pcm;
  pcm.reserve(static_cast<size_t>(out_frames) * 2);

  for (int frame = 0; frame < out_frames; ++frame) {
    double last = 0.0;
    for (int sub = 0; sub < kOversample; ++sub) {
      const int64_t next = static_cast<int64_t>((static_cast<double>(frame) * kOversample + sub + 1) * kSubWindow);
      while (wi < writes.size() && writes[wi].clock <= next) {
        const uint8_t d = writes[wi].data;
        if (d & 0x80) latch = (d >> 4) & 0x07;
        const bool tone = (latch == 0 || latch == 2 || latch == 4);
        if (tone) {
          if (d & 0x80) regs[latch] = (regs[latch] & 0x3f0) | (d & 0x0f);
          else regs[latch] = (regs[latch] & 0x00f) | ((d & 0x3f) << 4);
        } else {
          regs[latch] = d & 0x0f;
          if (latch == 6) lfsr = 0x8000;  // writing the noise control resets its shift register
        }
        ++wi;
      }
      const double W = std::max(1.0, static_cast<double>(next - clock));
      clock = next;
      double mix = 0.0;
      for (int ch = 0; ch < 3; ++ch) {
        const int n = regs[ch * 2] ? regs[ch * 2] : 1;
        mix += gain(regs[ch * 2 + 1]) * integrate_square(tone_phase[ch], n * kPsgRatio, W);
      }
      const int nf = regs[6] & 0x03;
      const bool white = (regs[6] & 0x04) != 0;
      const double noise_period = nf == 3 ? std::max(1, regs[4]) * kPsgRatio : (16 << nf) * kPsgRatio;
      mix += gain(regs[7]) * integrate_noise(noise_phase, noise_period, W, lfsr, white);
      last = mix;
      decim.push(mix * 9000.0);
    }
    (void)last;
    const double s = decim.read();
    pcm.push_back(yanes::replay::clip(s));
    pcm.push_back(yanes::replay::clip(s));
  }
  yanes::replay::wav(dir + "/yanes_sms_mix.wav", pcm, kRate);
  return 0;
}
