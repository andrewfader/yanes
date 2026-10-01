// Unit tests for the second wave of chip models in src/extra_chips.hpp: each checks the model
// against the hardware rule it claims to follow (a lattice that matches its own polynomial, an
// ADPCM codec that tracks its input, LFSR loops of the documented lengths, timers that only reach
// clock / integer rates, and so on).
#include "../src/extra_chips.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace X = yanes::extra;

namespace {

// |X(f)| of x at `hz`, by direct correlation.
double tone_level(const std::vector<double>& x, double rate, double hz) {
  double re = 0.0, im = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    re += x[i] * std::cos(X::kTau * hz * i / rate);
    im += x[i] * std::sin(X::kTau * hz * i / rate);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

bool is_clock_over_integer(double rate, double clock) {
  const double divisor = clock / rate;
  return std::abs(divisor - std::round(divisor)) < 1e-6;
}

void lpc_lattice_matches_its_polynomial() {
  // The step-down coefficients drive a lattice whose impulse response is exactly 1 / A(z).
  constexpr double rate = 8000.0;
  for (const double position : {0.0, 0.3, 0.5, 0.8, 1.0}) {
    const auto a = X::formant_polynomial(X::vowel_at(position), rate);
    X::LpcVoice lattice;
    lattice.k = X::reflection_coefficients(a);
    for (const double k : lattice.k) assert(std::abs(k) < 1.0 && "a formant filter is stable");
    std::vector<double> direct(200, 0.0);
    for (size_t n = 0; n < direct.size(); ++n) {
      double y = n == 0 ? 1.0 : 0.0;
      for (size_t i = 1; i <= X::kLpcOrder && i <= n; ++i) y -= a[i] * direct[n - i];
      direct[n] = y;
    }
    for (size_t n = 0; n < direct.size(); ++n) {
      const double y = lattice.lattice(n == 0 ? 1.0 : 0.0);
      assert(std::abs(y - direct[n]) < 1e-6 && "the lattice has the direct-form impulse response");
    }
  }
  // The formants it is built from are where its response peaks.
  {
    const auto a = X::formant_polynomial(X::vowel_at(0.5), rate);  // "ah": 730 Hz first formant
    auto gain = [&](double hz) {
      double re = 0.0, im = 0.0;
      for (size_t i = 0; i < a.size(); ++i) { re += a[i] * std::cos(X::kTau * hz * i / rate); im -= a[i] * std::sin(X::kTau * hz * i / rate); }
      return 1.0 / std::hypot(re, im);
    };
    assert(gain(730.0) > 4.0 * gain(400.0) && gain(730.0) > 4.0 * gain(1500.0) && "\"ah\" resonates at its first formant");
  }
}

void lpc_quantization_follows_the_chip_bit_depths() {
  std::vector<std::set<double>> seen(X::kLpcOrder);
  for (int i = 0; i <= 200; ++i) {
    const auto q = X::quantize_reflections(X::reflection_coefficients(X::formant_polynomial(X::vowel_at(i / 200.0), 8000.0)));
    for (size_t n = 0; n < q.size(); ++n) {
      assert(std::abs(q[n]) < 1.0 && "quantised coefficients stay stable");
      seen[n].insert(q[n]);
    }
  }
  for (size_t n = 0; n < seen.size(); ++n)
    assert(seen[n].size() <= static_cast<size_t>(1 << X::kLpcBits[n]) && "no more levels than the coefficient's bits");
  // A voiced lattice rings at the requested pitch.
  X::LpcVoice voice;
  voice.set_formants(X::vowel_at(0.5), 8000.0, true);
  std::vector<double> y;
  for (int i = 0; i < 8000; ++i) y.push_back(voice.tick(200.0, 8000.0, 1.0));
  assert(tone_level(y, 8000.0, 200.0) > 4.0 * tone_level(y, 8000.0, 290.0) && "the chirp train sets the pitch");
}

void oki_adpcm_tracks_its_input() {
  X::OkiAdpcm codec;
  double error = 0.0, energy = 0.0;
  for (int i = 0; i < 7575; ++i) {
    const int target = static_cast<int>(std::lround(1500.0 * std::sin(X::kTau * 220.0 * i / X::kOkiRate)));
    codec.encode(target);
    assert(codec.signal >= -2048 && codec.signal <= 2047 && "the signal is 12-bit");
    if (i > 200) { error += (codec.signal - target) * (codec.signal - target); energy += target * target; }
  }
  assert(error / energy < 0.02 && "4-bit ADPCM follows a sine to better than 17 dB SNR");
  // Silence lets the step size fall back to its smallest value.
  for (int i = 0; i < 400; ++i) codec.encode(0);
  assert(codec.index == 0 && std::abs(codec.signal) < 32);
  // Decoding is the published rule: a nibble moves the signal by (2m + 1) / 8 of the step.
  X::OkiAdpcm d;
  d.decode(7);
  assert(d.signal == (15 * 16) / 8 && d.index == 8);
  d.decode(8 | 3);
  assert(d.signal == 30 - (7 * 34) / 8 && d.index == 7);
}

void lynx_taps_loop_at_their_documented_lengths() {
  for (size_t shape = 0; shape < X::kLynxTaps.size(); ++shape) {
    uint16_t state = 0;
    bool bit = false;
    for (int i = 0; i < 64; ++i) state = X::lynx_lfsr_clock(state, X::kLynxTaps[shape], bit);  // settle
    std::map<uint16_t, int> first;
    int period = 0;
    for (int i = 0; i < 5000; ++i) {
      const auto [it, fresh] = first.emplace(state, i);
      if (!fresh) { period = i - it->second; break; }
      state = X::lynx_lfsr_clock(state, X::kLynxTaps[shape], bit);
    }
    if (shape + 1 < X::kLynxTaps.size()) assert(period == X::kLynxPeriods[shape] && "a tonal tap set loops as documented");
    else assert(period > 1000 && "the noise tap set does not repeat for over a thousand steps");
  }
  for (const double want : {261.63 * 2, 1000.0, 9000.0, 60000.0}) {
    const double rate = X::lynx_timer_rate(want);
    bool reachable = false;
    for (int prescale = 0; prescale <= 6; ++prescale) {
      const double clock = 1000000.0 / (1 << prescale);
      const double count = clock / rate;
      reachable = reachable || (std::abs(count - std::round(count)) < 1e-6 && count >= 1.0 && count <= 256.0);
    }
    assert(reachable && "Mikey's timer only reaches 1 MHz / 2^n / (1..256)");
    assert(std::abs(12.0 * std::log2(rate / want)) < 0.5 && "and gets within a quarter tone of the request");
  }
}

void timers_only_reach_whole_divisors() {
  for (const double hz : {55.0, 261.63, 440.0, 1046.5, 3520.0}) {
    assert(is_clock_over_integer(X::paula_rate(hz * 32), X::kPaulaClock));
    assert(is_clock_over_integer(X::pc_speaker_frequency(hz), 1193182.0));
    assert(is_clock_over_integer(X::zx_frequency(hz) * 104.0, 3500000.0));
    assert(is_clock_over_integer(X::apple2_frequency(hz) * 10.0, 1023000.0));
    assert(is_clock_over_integer(X::vsu_frequency(hz) * 32.0, 5000000.0));
    assert(is_clock_over_integer(X::wonderswan_frequency(hz) * 32.0, 3072000.0));
    assert(is_clock_over_integer(X::ay_envelope_frequency(hz, false) * 256.0, 2000000.0));
    assert(is_clock_over_integer(X::ay_envelope_frequency(hz, true) * 512.0, 2000000.0));
  }
  // Paula cannot go faster than ProTracker's shortest period.
  assert(std::abs(X::paula_rate(1.0e7) - X::kPaulaClock / X::kPaulaMinPeriod) < 1e-9);
}

void ay_envelope_runs_through_the_log_dac() {
  double mean = 0.0;
  for (const double level : X::kAyDac) mean += level / 16.0;
  assert(std::abs(mean - X::kAyDacMean) < 1e-5 && "the centring constant is the DAC table's mean");
  for (size_t i = 1; i < X::kAyDac.size(); ++i) assert(X::kAyDac[i] > X::kAyDac[i - 1] && "the DAC is monotonic");
  // Saws visit each of the 16 steps once per cycle; triangles twice, down then up.
  std::vector<int> down, up, tri;
  for (int i = 0; i < 32; ++i) {
    down.push_back(X::ay_envelope_step(0, (i + 0.5) / 32.0));
    up.push_back(X::ay_envelope_step(1, (i + 0.5) / 32.0));
    tri.push_back(X::ay_envelope_step(2, (i + 0.5) / 32.0));
  }
  assert(down.front() == 15 && down.back() == 0 && up.front() == 0 && up.back() == 15);
  for (int i = 0; i < 16; ++i) assert(tri[static_cast<size_t>(i)] == 15 - i && tri[static_cast<size_t>(31 - i)] == 15 - i);
}

void wavetables_have_the_consoles_sample_depths() {
  for (const bool wonderswan : {false, true}) {
    for (int shape = 0; shape < 8; ++shape) {
      std::set<float> levels;
      for (int i = 0; i < 32; ++i) {
        const float v = X::console_wavetable(shape, (i + 0.5) / 32.0, wonderswan ? 16 : 64, wonderswan);
        assert(v >= -1.0f && v <= 1.0f);
        const double step = (v + 1.0) / 2.0 * ((wonderswan ? 16 : 64) - 1);
        assert(std::abs(step - std::round(step)) < 1e-4 && "every sample sits on a DAC level");
        levels.insert(v);
      }
      assert(levels.size() >= 2);
    }
  }
  // The two consoles' default tables differ.
  bool differ = false;
  for (int i = 0; i < 32; ++i)
    differ = differ || X::console_wavetable(0, i / 32.0, 16, false) != X::console_wavetable(0, i / 32.0, 16, true);
  assert(differ);
}

void sample_loops_are_eight_bit() {
  for (int shape = 0; shape < 8; ++shape) {
    int lo = 127, hi = -128;
    for (int i = 0; i < X::kPaulaLoop; ++i) { lo = std::min<int>(lo, X::paula_builtin(shape, i)); hi = std::max<int>(hi, X::paula_builtin(shape, i)); }
    assert(hi - lo > 100 && "each Paula loop uses most of the 8-bit range");
    lo = 127; hi = -128;
    for (int i = 0; i < X::kGbaLoop; ++i) { lo = std::min<int>(lo, X::gba_builtin(shape, i)); hi = std::max<int>(hi, X::gba_builtin(shape, i)); }
    assert(hi - lo > 100 && "each GBA loop uses most of the 8-bit range");
  }
}

void arcade_kit_sounds_are_short_and_distinct() {
  std::vector<std::vector<double>> sounds;
  for (int sound = 0; sound < 12; ++sound) {
    uint32_t lfsr = 1;
    double previous = 0.0;
    X::LpcVoice speech;
    std::vector<double> x;
    for (double t = 0.0; t < X::kArcadeKitLength[static_cast<size_t>(sound)]; t += 1.0 / X::kOkiRate) {
      const double v = X::arcade_kit(sound, t, lfsr, previous, speech);
      assert(std::isfinite(v));
      x.push_back(v);
    }
    double energy = 0.0;
    for (const double v : x) energy += v * v;
    assert(energy / static_cast<double>(x.size()) > 1e-4 && "every kit sound is audible");
    assert(X::kArcadeKitLength[static_cast<size_t>(sound)] < 1.0 && "and over within a second");
    sounds.push_back(x);
  }
  // The shouts are voiced: they carry their pitch, which the drums do not.
  assert(tone_level(std::vector<double>(sounds[9].begin() + 600, sounds[9].begin() + 1600), X::kOkiRate, 175.0) >
         2.0 * tone_level(std::vector<double>(sounds[9].begin() + 600, sounds[9].begin() + 1600), X::kOkiRate, 260.0));
}

void slap_bass_overtones_die_before_the_fundamental() {
  constexpr double rate = 48000.0, hz = 82.41;  // low E
  auto window = [&](double age, double pop) {
    std::vector<double> x;
    for (int i = 0; i < 4800; ++i) {
      const double t = age + i / rate;
      x.push_back(X::slap_bass(hz * t, t, pop, 0.0));
    }
    return x;
  };
  for (const double pop : {0.0, 1.0}) {
    const auto early = window(0.01, pop), late = window(0.6, pop);
    const double early_ratio = tone_level(early, rate, hz * 4) / tone_level(early, rate, hz);
    const double late_ratio = tone_level(late, rate, hz * 4) / tone_level(late, rate, hz);
    assert(late_ratio < 0.5 * early_ratio && "the string darkens as it rings");
    assert(tone_level(late, rate, hz) > 0.25 * tone_level(early, rate, hz) && "the fundamental is still ringing");
  }
  // A pop is brighter than a thumb.
  assert(tone_level(window(0.01, 1.0), rate, hz * 6) > tone_level(window(0.01, 0.0), rate, hz * 6));
}

}  // namespace

int main() {
  lpc_lattice_matches_its_polynomial();
  lpc_quantization_follows_the_chip_bit_depths();
  oki_adpcm_tracks_its_input();
  lynx_taps_loop_at_their_documented_lengths();
  timers_only_reach_whole_divisors();
  ay_envelope_runs_through_the_log_dac();
  wavetables_have_the_consoles_sample_depths();
  sample_loops_are_eight_bit();
  arcade_kit_sounds_are_short_and_distinct();
  slap_bass_overtones_die_before_the_fundamental();
  std::printf("extra chip model tests passed\n");
}
