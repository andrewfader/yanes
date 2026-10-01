// SID reference parity: compares the plug-in's SID voice against reSIDfp, the
// reference MOS6581/8580 emulation (more accurate than a chip-model comparison
// against Furnace). The same note and waveform is rendered through both and the
// steady-state waveform shapes are cross-correlated. The reSIDfp reference lives
// in sid_resid_reference.cpp and is linked only when libresidfp is available.
//
// YANES's SID is an original musical model, not a reSID clone, so this is a
// benchmark of how close it sits to the reference rather than a bit-exact gate;
// the pure oscillator waveforms (saw, triangle, pulse) are what it must get right.
#include "clap_harness.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

std::vector<float> sid_reference_render(int chip, int wave, double hz, double pw, int frames);

using namespace harness;

namespace {

std::vector<float> yanes_render(const Library& library, int waveform, int shape, int key, int frames) {
  const clap_plugin_t* plugin = library.create();
  std::vector<float> out;
  {
    Runner runner(plugin, 44100.0, 256);
    runner.set(find_param(plugin, "Waveform"), waveform);
    runner.set(find_param(plugin, "Shape"), shape);
    runner.set(find_param(plugin, "Pulse duty"), 2);        // 50% for the pulse case
    runner.set(find_param(plugin, "Chip cutoff"), 16000);   // filter wide open
    runner.set(find_param(plugin, "Chip resonance"), 0);
    runner.set(find_param(plugin, "Attack"), 0);
    runner.set(find_param(plugin, "Release"), 2000);
    runner.set(find_param(plugin, "Voice gain"), 0);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(key), 1, 1.0));
    for (int rendered = 0; rendered < frames; rendered += 256) {
      runner.run(rendered == 0 ? &on : nullptr);
      for (uint32_t i = 0; i < 256 && static_cast<int>(out.size()) < frames; ++i) out.push_back(runner.left()[i]);
    }
  }
  plugin->destroy(plugin);
  return out;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0; for (size_t i = from; i < to && i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / std::max<size_t>(1, to - from));
}

double mean(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0; for (size_t i = from; i < to && i < x.size(); ++i) s += x[i];
  return s / std::max<size_t>(1, to - from);
}

// Best lag Pearson correlation over a steady window. The mean is removed because
// the 8580's output carries a DC offset the real console's output capacitor (and
// the plug-in's DC blocker) both discard; timbre, not DC, is what is compared.
double best_correlation(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t from = 1500, len = 4000;
  const double ma = mean(a, from, from + len), mb = mean(b, from, from + len);
  double best = -1.0;
  for (int lag = -600; lag <= 600; ++lag) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < len; ++i) {
      const size_t ia = from + i; const long ib = static_cast<long>(from + i) + lag;
      if (ib < 0 || static_cast<size_t>(ib) >= b.size()) continue;
      const double va = a[ia] - ma, vb = b[static_cast<size_t>(ib)] - mb;
      dot += va * vb; na += va * va; nb += vb * vb;
    }
    if (na > 1e-9 && nb > 1e-9) best = std::max(best, dot / std::sqrt(na * nb));
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: sid_resid_parity <clap>\n"); return 2; }
  const Library library(argv[1]);
  const int key = 60, frames = 8000;
  const double hz = 440.0 * std::exp2((key - 69) / 12.0);

  struct Case { const char* name; int wave_bits; int shape; };
  const std::array<Case, 3> cases = {{{"sawtooth", 0x20, 0}, {"triangle", 0x10, 1}, {"pulse 50%", 0x40, 2}}};
  // reSIDfp's minimum gate per waveform (it has DAC nonlinearity and, on the
  // 6581, a resistive-ladder droop the plug-in only approximates).
  const double gate = 0.80;
  int failures = 0;
  double worst = 1.0;
  for (int chip = 0; chip < 2; ++chip) {
    std::printf("%s:\n", chip == 0 ? "MOS6581" : "MOS8580");
    for (const Case& c : cases) {
      const auto reference = sid_reference_render(chip, c.wave_bits, hz, 0.5, frames);
      const auto mine = yanes_render(library, chip == 0 ? 38 : 39, c.shape, key, frames);
      const double corr = best_correlation(reference, mine);
      std::printf("  %-10s correlation %.4f\n", c.name, corr);
      if (!(corr > gate)) ++failures;
      worst = std::min(worst, corr);
    }
  }
  if (failures) {
    std::fprintf(stderr, "SID reference parity FAILED: %d waveform(s) below %.2f (worst %.4f)\n", failures, gate, worst);
    return 1;
  }
  std::printf("SID reference parity: every waveform tracks reSIDfp (worst correlation %.4f)\n", worst);
  return 0;
}
