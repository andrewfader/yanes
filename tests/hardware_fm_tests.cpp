#include "hardware_fm.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using Voice = yanes::HardwareFmVoice;
constexpr double rate = 48000;
yanes::FmControls controls() {
  yanes::FmControls c;
  c.algorithm = 7; c.feedback = 0; c.decay = 0; c.sustain_rate = 0;
  c.sustain_level = 0; c.key_scale = 0; c.brightness = 1;
  return c;
}
std::vector<float> render(Voice::Kind kind, double hz, const yanes::FmControls& c) {
  Voice voice;
  voice.prepare();
  voice.key_on(kind, hz, c);
  for (int i = 0; i < 4800; ++i) voice.render(rate, hz);
  std::vector<float> samples(12000);
  for (auto& x : samples) { x = voice.render(rate, hz); assert(std::isfinite(x)); }
  return samples;
}
double amplitude(const std::vector<float>& samples, double hz) {
  double best = 0;
  // Permit each chip's finite pitch resolution without folding away octave errors.
  for (double detune : {-2.0, 0.0, 2.0}) {
    double re = 0, im = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      const double a = 6.283185307179586 * (hz + detune) * i / rate;
      re += samples[i] * std::cos(a); im += samples[i] * std::sin(a);
    }
    best = std::max(best, std::hypot(re, im) * 2 / samples.size());
  }
  return best;
}
int main() {
  for (const auto kind : {Voice::Kind::Ym2612, Voice::Kind::Opn, Voice::Kind::Opna,
       Voice::Kind::Opl2, Voice::Kind::Opl3, Voice::Kind::Opm, Voice::Kind::Opl3FourOp}) {
    for (const double hz : {440.0, 3520.0}) {
      const auto samples = render(kind, hz, controls());
      const double wanted = amplitude(samples, hz), sub = amplitude(samples, hz / 2);
      if (!(wanted > 0.025 && sub < wanted * 0.15)) {
        std::fprintf(stderr, "kind=%d hz=%g wanted=%g sub=%g\n", static_cast<int>(kind), hz, wanted, sub);
        assert(false && "absolute FM pitch must match the requested note");
      }
    }
  }
  for (bool pm : {false, true}) {
    auto c = controls(); c.lfo_rate = 7;
    const auto clean = render(Voice::Kind::Opm, 440, c);
    if (pm) c.pm_depth = 127; else c.am_depth = 127;
    const auto modulated = render(Voice::Kind::Opm, 440, c);
    double difference = 0;
    for (size_t i = 0; i < clean.size(); ++i) difference += std::abs(clean[i] - modulated[i]);
    assert(difference / clean.size() > 0.01 && "OPM modulation depth must reach the channel sensitivity registers");
  }
  std::puts("hardware_fm_tests: absolute pitch and OPM modulation passed");
}
