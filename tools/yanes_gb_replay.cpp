// Replays a SameBoy APU register log through YANES's own DSP primitives.
//
// This is the thing under test. The oracle (tools/yanes_gb_oracle.c) produced
// the register stream and SameBoy's reference audio from one run of a real ROM;
// this tool drives the same registers into the oscillators the plugin ships
// (src/dsp.hpp) and renders what YANES would make of them. A divergence scored
// against the oracle is a real defect in the plugin's synthesis path, not a
// fixture artefact.
//
// The register decode here -- frame sequencer, envelope, sweep, length, wave RAM
// -- is the DMG control layer the plugin's parameter model does not yet express.
// It is written against the published DMG APU behaviour, not copied from
// SameBoy, so that the two sides stay independent.
//
// Each channel is a counter driving a small digital level into its own DAC, so
// the waveform is a staircase whose steps land on master-clock edges, not on
// host sample boundaries. Reading each channel once per output sample folds
// those edges back into the audible band: a wave channel playing a 1 kHz note
// steps 32000 times a second, and at 48 kHz that is an alias, not a note. The
// render integrates every channel exactly between its own steps and decimates
// through a windowed-sinc filter, which is the band-limiting the oracle's own
// output path performs.

#include "../src/dsp.hpp"
#include "replay_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kDmgClock = 4194304.0;
constexpr int kSampleRate = 48000;
constexpr int kOversample = 4;
constexpr int kSubRate = kSampleRate * kOversample;
// The frame sequencer runs at 512 Hz: length at 256 Hz, sweep at 128 Hz and the
// volume envelope at 64 Hz.
constexpr int64_t kSequencerPeriod = 8192;
constexpr std::array<double, 4> kDuty{0.125, 0.25, 0.5, 0.75};
constexpr std::array<int, 8> kNoiseDivisors{8, 16, 32, 48, 64, 80, 96, 112};

struct Write { uint64_t sample; uint8_t reg, value; };

// Each DMG channel drives its own DAC from a unipolar 0..15 digital level, so a
// running channel whose volume is zero, or one whose length counter has expired,
// still sits at the bottom of that range rather than at silence. Returning a
// hard zero for those cases loses the level step the hardware makes audible --
// and makes an isolated-channel comparison against the oracle meaningless, since
// the oracle keeps the bias. A DAC that is switched off contributes nothing.
inline double dmg_dac(int digital) { return digital / 7.5 - 1.0; }

struct Envelope {
  int volume{0}, initial{0}, period{0}, timer{0};
  bool increase{false};

  void load(uint8_t nrx2) {
    initial = nrx2 >> 4;
    increase = (nrx2 & 0x08) != 0;
    period = nrx2 & 0x07;
  }
  void trigger() { volume = initial; timer = period; }
  void tick() {
    if (period == 0) return;
    if (--timer > 0) return;
    timer = period;
    const int next = volume + (increase ? 1 : -1);
    if (next >= 0 && next <= 15) volume = next;
  }
  // The DAC is off when the upper five bits of NRx2 are clear, which silences
  // the channel regardless of the envelope's current level.
  bool dac_enabled() const { return initial != 0 || increase; }
};

// Common timing state: every channel is a countdown to its next step.
struct Voice {
  yanes::replay::Integrator dac;
  int64_t next_step{0};
  bool active{false};
};

struct Pulse : Voice {
  int position{0};           // Duty step, 0..7.
  int duty{2};
  int frequency{0};          // 11-bit register value
  int length{0};
  bool length_enabled{false};
  Envelope envelope;

  // Sweep (CH1 only).
  bool sweep_enabled{false};
  int sweep_period{0}, sweep_shift{0}, sweep_timer{0}, sweep_shadow{0};
  bool sweep_decrease{false};

  // One duty step every (2048 - frequency) * 4 clocks; eight steps to a cycle.
  int64_t period() const { return static_cast<int64_t>(2048 - frequency) * 4; }
  void step() { position = (position + 1) & 7; }

  void trigger(int64_t clock) {
    active = envelope.dac_enabled();
    envelope.trigger();
    if (length == 0) length = 64;
    next_step = clock + period();
    sweep_shadow = frequency;
    sweep_timer = sweep_period ? sweep_period : 8;
    sweep_enabled = sweep_period != 0 || sweep_shift != 0;
    if (sweep_shift != 0) sweep_step(false);
  }

  // NR10 walks the frequency register itself: f +/- (f >> shift). An overflow
  // upward disables the channel, which is how the hardware ends a rising sweep.
  int sweep_target() const {
    const int delta = sweep_shadow >> sweep_shift;
    return sweep_decrease ? sweep_shadow - delta : sweep_shadow + delta;
  }
  void sweep_step(bool commit) {
    const int target = sweep_target();
    if (target > 2047) { active = false; return; }
    if (commit && sweep_shift != 0) {
      sweep_shadow = target;
      frequency = target;
    }
  }
  void tick_sweep() {
    if (!sweep_enabled) return;
    if (--sweep_timer > 0) return;
    sweep_timer = sweep_period ? sweep_period : 8;
    if (sweep_period == 0) return;
    sweep_step(true);
    sweep_step(false);
  }
  void tick_length() {
    if (!length_enabled || length == 0) return;
    if (--length == 0) active = false;
  }

  double analog() const {
    if (!envelope.dac_enabled()) return 0.0;
    if (!active) return dmg_dac(0);
    // The plugin's own two-level pulse, sampled at the duty step the counter is
    // holding: a mismatch here is a plugin defect.
    const double phase = (position + 0.5) / 8.0;
    const bool high =
        yanes::pulse_raw(phase, kDuty[static_cast<size_t>(duty)]) > 0.0f;
    return dmg_dac(high ? envelope.volume : 0);
  }
};

struct Wave : Voice {
  int position{0};           // Wave-RAM step, 0..31.
  int frequency{0}, length{0}, volume_code{0};
  bool length_enabled{false}, dac_on{false};
  std::array<uint8_t, 16> ram{};

  // Wave RAM is read twice as often as a pulse channel steps its duty.
  int64_t period() const { return static_cast<int64_t>(2048 - frequency) * 2; }
  void step() { position = (position + 1) & 31; }
  void trigger(int64_t clock) {
    active = dac_on;
    if (length == 0) length = 256;
    position = 0;
    next_step = clock + period();
  }
  void tick_length() {
    if (!length_enabled || length == 0) return;
    if (--length == 0) active = false;
  }
  double analog() const {
    if (!dac_on) return 0.0;
    if (!active) return dmg_dac(0);
    // NR32 shifts the four-bit sample right, so the quiet settings move the
    // level towards the bottom of the DAC range rather than towards its centre.
    const double phase = (position + 0.5) / 32.0;
    const float sample = yanes::game_boy_wave_sample(phase, ram.data());
    const int nibble =
        std::clamp(static_cast<int>(std::lround((sample + 1.0f) * 7.5f)), 0, 15);
    return dmg_dac(volume_code ? nibble >> (volume_code - 1) : 0);
  }
};

struct Noise : Voice {
  uint32_t lfsr{0x7fff};
  int divisor_code{0}, shift{0}, length{0};
  bool narrow{false}, length_enabled{false};
  Envelope envelope;

  // Shift counts of 14 and 15 are not produced by the divider, and the hardware
  // simply stops clocking the register.
  int64_t period() const {
    if (shift >= 14) return 0;
    return static_cast<int64_t>(kNoiseDivisors[static_cast<size_t>(divisor_code)])
           << shift;
  }
  void step() { lfsr = yanes::game_boy_lfsr_clock(lfsr, narrow); }
  void trigger(int64_t clock) {
    active = envelope.dac_enabled();
    envelope.trigger();
    if (length == 0) length = 64;
    lfsr = 0x7fff;
    next_step = clock + std::max<int64_t>(period(), 1);
  }
  void tick_length() {
    if (!length_enabled || length == 0) return;
    if (--length == 0) active = false;
  }
  // The channel outputs the inverse of the register's low bit.
  double analog() const {
    if (!envelope.dac_enabled()) return 0.0;
    if (!active) return dmg_dac(0);
    return dmg_dac((lfsr & 1U) ? 0 : envelope.volume);
  }
};

struct Apu {
  Pulse ch1, ch2;
  Wave ch3;
  Noise ch4;
  uint8_t nr50{0x77}, nr51{0xf3};
  bool enabled{false};
  int sequencer_step{0};
  int64_t clock{0}, sequencer_next{kSequencerPeriod};

  // NR51 routes each channel to either side; NR50 sets a 3-bit master level per
  // side, and the chip has no zero-volume step.
  double routed(int index, double analog, int side) const {
    const int bit = side == 0 ? index + 4 : index;
    if (!(nr51 & (1u << bit))) return 0.0;
    const int master = side == 0 ? ((nr50 >> 4) & 0x07) : (nr50 & 0x07);
    return analog * (master + 1);
  }

  template <class Channel>
  void refresh(Channel& ch, int index) {
    const double analog = ch.analog();
    ch.dac.level[0] = routed(index, analog, 0);
    ch.dac.level[1] = routed(index, analog, 1);
  }

  // Steps one channel's counter up to `target`, closing off the exact span it
  // held each level for.
  template <class Channel>
  void advance(Channel& ch, int index, int64_t target) {
    refresh(ch, index);
    while (ch.next_step <= target) {
      const int64_t period = ch.period();
      if (period <= 0) {
        ch.next_step = target + 1;
        break;
      }
      ch.dac.hold_until(ch.next_step);
      ch.step();
      refresh(ch, index);
      ch.next_step += period;
    }
    ch.dac.hold_until(target);
  }

  void run_to(int64_t target) {
    while (clock < target) {
      const int64_t next = std::min(target, sequencer_next);
      advance(ch1, 0, next);
      advance(ch2, 1, next);
      advance(ch3, 2, next);
      advance(ch4, 3, next);
      clock = next;
      if (clock == sequencer_next) {
        tick_sequencer();
        sequencer_next += kSequencerPeriod;
      }
    }
  }

  void write(int64_t at, uint8_t reg, uint8_t value) {
    run_to(at);
    // With NR52 bit 7 clear the APU ignores everything but NR52 itself.
    if (!enabled && reg != 0x26 && reg < 0x30) return;
    switch (reg) {
      case 0x10:
        ch1.sweep_period = (value >> 4) & 0x07;
        ch1.sweep_decrease = (value & 0x08) != 0;
        ch1.sweep_shift = value & 0x07;
        break;
      case 0x11: ch1.duty = value >> 6; ch1.length = 64 - (value & 0x3f); break;
      case 0x12: ch1.envelope.load(value); if (!ch1.envelope.dac_enabled()) ch1.active = false; break;
      case 0x13: ch1.frequency = (ch1.frequency & 0x700) | value; break;
      case 0x14:
        ch1.frequency = (ch1.frequency & 0xff) | ((value & 0x07) << 8);
        ch1.length_enabled = (value & 0x40) != 0;
        if (value & 0x80) ch1.trigger(at);
        break;
      case 0x16: ch2.duty = value >> 6; ch2.length = 64 - (value & 0x3f); break;
      case 0x17: ch2.envelope.load(value); if (!ch2.envelope.dac_enabled()) ch2.active = false; break;
      case 0x18: ch2.frequency = (ch2.frequency & 0x700) | value; break;
      case 0x19:
        ch2.frequency = (ch2.frequency & 0xff) | ((value & 0x07) << 8);
        ch2.length_enabled = (value & 0x40) != 0;
        if (value & 0x80) ch2.trigger(at);
        break;
      case 0x1a: ch3.dac_on = (value & 0x80) != 0; if (!ch3.dac_on) ch3.active = false; break;
      case 0x1b: ch3.length = 256 - value; break;
      case 0x1c: ch3.volume_code = (value >> 5) & 0x03; break;
      case 0x1d: ch3.frequency = (ch3.frequency & 0x700) | value; break;
      case 0x1e:
        ch3.frequency = (ch3.frequency & 0xff) | ((value & 0x07) << 8);
        ch3.length_enabled = (value & 0x40) != 0;
        if (value & 0x80) ch3.trigger(at);
        break;
      case 0x20: ch4.length = 64 - (value & 0x3f); break;
      case 0x21: ch4.envelope.load(value); if (!ch4.envelope.dac_enabled()) ch4.active = false; break;
      case 0x22:
        ch4.shift = value >> 4;
        ch4.narrow = (value & 0x08) != 0;
        ch4.divisor_code = value & 0x07;
        break;
      case 0x23:
        ch4.length_enabled = (value & 0x40) != 0;
        if (value & 0x80) ch4.trigger(at);
        break;
      case 0x24: nr50 = value; break;
      case 0x25: nr51 = value; break;
      case 0x26:
        enabled = (value & 0x80) != 0;
        if (!enabled) { ch1.active = ch2.active = ch3.active = ch4.active = false; }
        break;
      default:
        if (reg >= 0x30 && reg <= 0x3f) ch3.ram[reg - 0x30] = value;
        break;
    }
  }

  void tick_sequencer() {
    if ((sequencer_step & 1) == 0) {
      ch1.tick_length(); ch2.tick_length(); ch3.tick_length(); ch4.tick_length();
    }
    if (sequencer_step == 2 || sequencer_step == 6) ch1.tick_sweep();
    if (sequencer_step == 7) {
      ch1.envelope.tick(); ch2.envelope.tick(); ch4.envelope.tick();
    }
    sequencer_step = (sequencer_step + 1) & 7;
  }

  // Mean level each channel held over the window ending at `to`.
  void drain(int64_t to, double (&out)[4][2]) {
    const double span = static_cast<double>(to - window_start);
    window_start = to;
    run_to(to);
    Voice* voices[4] = {&ch1, &ch2, &ch3, &ch4};
    for (int index = 0; index < 4; ++index)
      for (int side = 0; side < 2; ++side)
        out[index][side] = voices[index]->dac.mean(side, span);
  }

  int64_t window_start{0};
};

using Decimator = yanes::replay::Decimator<kOversample>;

// One pass produces the mix and every isolated channel, mirroring the oracle's
// GB_set_channel_muted passes without re-running the state machine four times.
struct Render {
  std::vector<int16_t> mix;
  std::array<std::vector<int16_t>, 4> solo;
};

Render render(const std::vector<Write>& writes, uint64_t total) {
  Apu apu;
  Render out;
  out.mix.reserve(static_cast<size_t>(total) * 2);
  for (auto& channel : out.solo) channel.reserve(static_cast<size_t>(total) * 2);

  std::array<std::array<Decimator, 2>, 4> decimate{};
  std::array<Decimator, 2> mix_decimate{};
  size_t next = 0;
  double window[4][2];
  // Four channels at full DAC swing and the loudest master level.
  constexpr double kScale = 32767.0 / (4.0 * 8.0);

  for (uint64_t sample = 0; sample < total; ++sample) {
    double mixed[2][kOversample];
    double soloed[4][2][kOversample];
    for (int sub = 0; sub < kOversample; ++sub) {
      const int64_t clock = static_cast<int64_t>(std::llround(
          (sample * kOversample + sub + 1.0) * kDmgClock / kSubRate));
      while (next < writes.size()) {
        const int64_t at = static_cast<int64_t>(
            std::llround(writes[next].sample * kDmgClock / kSampleRate));
        if (at > clock) break;
        apu.write(at, writes[next].reg, writes[next].value);
        ++next;
      }
      apu.drain(clock, window);
      for (int side = 0; side < 2; ++side) {
        double sum = 0;
        for (int index = 0; index < 4; ++index) {
          sum += window[index][side];
          soloed[index][side][sub] = window[index][side];
        }
        mixed[side][sub] = sum;
      }
    }
    for (int side = 0; side < 2; ++side) {
      for (int sub = 0; sub < kOversample; ++sub)
        mix_decimate[side].push(mixed[side][sub]);
      out.mix.push_back(yanes::replay::clip(mix_decimate[side].read() * kScale));
    }
    for (int index = 0; index < 4; ++index)
      for (int side = 0; side < 2; ++side) {
        for (int sub = 0; sub < kOversample; ++sub)
          decimate[index][side].push(soloed[index][side][sub]);
        out.solo[index].push_back(
            yanes::replay::clip(decimate[index][side].read() * kScale));
      }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: yanes-gb-replay <gb_registers.log> <out_dir> [seconds]\n"
                 "  Writes yanes_mix.wav and yanes_ch1..4.wav to <out_dir>.\n";
    return 2;
  }
  const std::string log_path = argv[1], out_dir = argv[2];
  const double seconds = argc >= 4 ? std::atof(argv[3]) : 0.0;

  std::ifstream in(log_path);
  if (!in) { std::cerr << "cannot open " << log_path << "\n"; return 1; }
  std::vector<Write> writes;
  std::string line;
  uint64_t last = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    uint64_t sample; std::string reg, value;
    if (!(row >> sample >> reg >> value)) continue;
    writes.push_back({sample, static_cast<uint8_t>(std::stoul(reg, nullptr, 16)),
                      static_cast<uint8_t>(std::stoul(value, nullptr, 16))});
    last = std::max(last, sample);
  }
  if (writes.empty()) { std::cerr << "no register writes in " << log_path << "\n"; return 1; }

  const uint64_t total = seconds > 0.0
      ? static_cast<uint64_t>(seconds * kSampleRate)
      : last + kSampleRate;

  const Render out = render(writes, total);
  yanes::replay::wav(out_dir + "/yanes_mix.wav", out.mix, kSampleRate);
  static const char* names[4] = {"ch1_pulse", "ch2_pulse", "ch3_wave", "ch4_noise"};
  for (int ch = 0; ch < 4; ++ch)
    yanes::replay::wav(out_dir + "/yanes_" + names[ch] + ".wav", out.solo[ch],
                       kSampleRate);
  std::cout << "replayed " << writes.size() << " register writes over "
            << static_cast<double>(total) / kSampleRate << "s -> " << out_dir << "\n";
  return 0;
}
