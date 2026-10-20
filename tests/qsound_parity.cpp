// QSound reference parity: compares the plug-in's Capcom QSound voice against superctr's DL-1425
// emulator (qsound_reference.cpp), the core MAME and Furnace run. Both play the same built-in
// 8-bit loop at the same rate register, and the plug-in runs at the chip's own 24038 Hz, so each
// output sample is one DSP step on both sides. The score is the signal-to-error ratio after the
// best lag and gain, over a full second (a pitch a couple of cents off drifts a whole step out of
// phase in that time). Two claims, two gates:
//
// 1. The voice is the chip's, sample for sample. The reference's linear-pan output is the bare
//    voice (delayed); run through the plug-in's own Q1 model, it must match the plug-in's output
//    to 35 dB on each side. A wrong loop, a 13379 Hz mix, a rate off the 1/4096 grid or linear
//    interpolation all fall below 30 dB.
// 2. The Q1 model is the chip's output stage. Against the reference's own Q1 output at the
//    power-on centre pan, each side must clear 15 dB: the model leaves out the filter's fine ripple
//    and sits at 17-27 dB, while no Q1 stage at all scores 6-12 dB.
#include "clap_harness.hpp"
#include "extra_chips.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

void qsound_reference_render(const std::vector<int8_t>& loop, uint16_t rate, bool surround, int frames,
                             std::vector<float>& left, std::vector<float>& right);

using namespace harness;

namespace {

constexpr double kChipRate = 60000000.0 / 2.0 / 1248.0;  // the DSP's 1248-cycle loop at 60 MHz

struct Stereo { std::vector<float> left, right; };

Stereo yanes_render(const Library& library, int shape, int key, int frames) {
  const clap_plugin_t* plugin = library.create();
  Stereo out;
  {
    Runner runner(plugin, kChipRate, 256);
    runner.set(find_param(plugin, "Waveform"), 84);
    runner.set(find_param(plugin, "Shape"), shape);
    runner.set(find_param(plugin, "Attack"), 0);
    runner.set(find_param(plugin, "Release"), 2000);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(key), 1, 1.0));
    for (int rendered = 0; rendered < frames; rendered += 256) {
      runner.run(rendered == 0 ? &on : nullptr);
      for (uint32_t i = 0; i < 256 && static_cast<int>(out.left.size()) < frames; ++i) {
        out.left.push_back(runner.left()[i]);
        out.right.push_back(runner.right()[i]);
      }
    }
  }
  plugin->destroy(plugin);
  return out;
}

double mean(const std::vector<float>& x, size_t from, size_t len) {
  double s = 0; for (size_t i = from; i < from + len; ++i) s += x[i];
  return s / static_cast<double>(len);
}

// Signal-to-error ratio in dB after the best lag and least-squares gain, over one second, DC
// removed (the plug-in's output DC blocker).
double best_snr(const std::vector<float>& ref, const std::vector<float>& mine) {
  const size_t from = 2400, len = 24038;
  const double mr = mean(ref, from, len), mm = mean(mine, from, len);
  double best = -100.0;
  for (int lag = -200; lag <= 200; ++lag) {
    double rr = 0, mmx = 0, rm = 0;
    for (size_t i = 0; i < len; ++i) {
      const long j = static_cast<long>(from + i) + lag;
      if (j < 0 || static_cast<size_t>(j) >= mine.size()) continue;
      const double r = ref[from + i] - mr, m = mine[static_cast<size_t>(j)] - mm;
      rr += r * r; mmx += m * m; rm += r * m;
    }
    if (rr < 1e-12 || mmx < 1e-12) continue;
    const double error = rr - rm * rm / mmx;  // residual after the best gain rm / mmx
    best = std::max(best, 10.0 * std::log10(rr / std::max(error, 1e-12 * rr)));
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: qsound_parity <clap>\n"); return 2; }
  const Library library(argv[1]);
  const int frames = 2400 + 24038 + 400;
  const double voice_gate = 35.0, q1_gate = 15.0;  // dB
  int failures = 0;
  double worst_voice = 1e9, worst_q1 = 1e9;
  for (const int shape : {0, 1, 7}) {
    std::vector<int8_t> loop;
    for (int i = 0; i < yanes::extra::kQsoundLoop; ++i) loop.push_back(yanes::extra::qsound_builtin(shape, i));
    for (const int key : {48, 60, 72}) {
      const double hz = 440.0 * std::exp2((key - 69) / 12.0);
      const auto rate = static_cast<uint16_t>(std::lround(hz * yanes::extra::kQsoundLoop / kChipRate * 4096.0));
      const Stereo mine = yanes_render(library, shape, key, frames);
      // 1. The bare voice, through the plug-in's Q1 model.
      std::vector<float> voice, unused;
      qsound_reference_render(loop, rate, false, frames, voice, unused);
      Stereo modelled;
      yanes::extra::QsoundQ1 q1;
      for (const float x : voice) {
        const auto out = q1.tick(x);
        modelled.left.push_back(static_cast<float>(out[0]));
        modelled.right.push_back(static_cast<float>(out[1]));
      }
      const double vl = best_snr(modelled.left, mine.left), vr = best_snr(modelled.right, mine.right);
      // 2. The chip's own Q1 stage.
      std::vector<float> left, right;
      qsound_reference_render(loop, rate, true, frames, left, right);
      const double ql = best_snr(left, mine.left), qr = best_snr(right, mine.right);
      std::printf("loop %d key %d (rate 0x%04x)  voice %.1f / %.1f dB   Q1 %.1f / %.1f dB (left / right)\n",
                  shape, key, rate, vl, vr, ql, qr);
      for (const double snr : {vl, vr}) {
        if (!(snr > voice_gate)) ++failures;
        worst_voice = std::min(worst_voice, snr);
      }
      for (const double snr : {ql, qr}) {
        if (!(snr > q1_gate)) ++failures;
        worst_q1 = std::min(worst_q1, snr);
      }
    }
  }
  if (failures) {
    std::fprintf(stderr, "QSound reference parity FAILED: %d side(s) below the gates (worst voice %.1f dB of %.0f, worst Q1 %.1f dB of %.0f)\n",
                 failures, worst_voice, voice_gate, worst_q1, q1_gate);
    return 1;
  }
  std::printf("QSound reference parity: voice worst %.1f dB, Q1 worst %.1f dB against the DL-1425 emulator\n", worst_voice, worst_q1);
  return 0;
}
