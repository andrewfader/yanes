// Every parameter and every preset, proved against what it advertises, from audio.
//
// The plug-in advertises each parameter as a name, a unit and a range (the host
// shows "Attack 120.0 ms", "Transpose +12", "Pulse duty 25%"), and each preset as
// a name. This suite holds one proof per parameter ID and one per preset. Each
// proof renders through the public CLAP interface and measures the advertised
// quantity in its own unit: milliseconds for times, dB for levels, cents and
// semitones for pitch, Hz for rates, the duty fraction, harmonic content for
// waveforms.
//
// The list is closed by construction. The proof tables are indexed by ID, and the
// suite fails if any parameter or preset has none, so a new parameter or preset
// cannot ship without a proof.
//
//   advertised_tests <YANES.clap> [name-filter]

#include "audio_measure.hpp"
#include "clap_harness.hpp"
#include "../src/params.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace harness;
namespace P = yanes::params;
using measure::Span;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;
const char* g_clap_path = nullptr;

// ----- rendering --------------------------------------------------------------

struct Setting { uint32_t id; double value; };
using Patch = std::vector<Setting>;

struct Event {
  enum Kind { kOn, kOff, kBend, kTempo, kMidi };
  double time;
  Kind kind;
  int channel = 0, key = 60;
  double value = 1.0;  // velocity, bend (-1..1) or tempo
  uint8_t midi[3] = {0, 0, 0};
};
using Score = std::vector<Event>;

Event on(double t, int key = 60, int channel = 0, double velocity = 1.0) {
  return {t, Event::kOn, channel, key, velocity};
}
Event off(double t, int key = 60, int channel = 0) { return {t, Event::kOff, channel, key, 0.0}; }
Event bend(double t, double amount, int channel = 0) { return {t, Event::kBend, channel, 0, amount}; }
Event tempo(double t, double bpm) { return {t, Event::kTempo, 0, 0, bpm}; }

struct Take {
  std::vector<float> left, right;
  std::vector<float> mono() const {
    std::vector<float> m(left.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (left[i] + right[i]);
    return m;
  }
  size_t at(double seconds) const { return std::min(left.size(), static_cast<size_t>(seconds * kRate)); }
};

// A fresh instance, the patch applied in order the way a host sets parameters
// (so a Waveform change brings its voice defaults before later settings), then
// the score rendered sample-accurately.
Take render(const Patch& patch, Score score, double seconds) {
  Library library(g_clap_path);
  const clap_plugin_t* plugin = library.create();
  for (const auto& s : patch) set_param(plugin, s.id, s.value);
  std::stable_sort(score.begin(), score.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
  Take take;
  {
    Runner runner(plugin, kRate, kBlock);
    const size_t frames = static_cast<size_t>(seconds * kRate);
    size_t next = 0;
    for (size_t at = 0; at < frames; at += kBlock) {
      Events events;
      while (next < score.size() && static_cast<size_t>(score[next].time * kRate) < at + kBlock) {
        const Event& e = score[next++];
        const uint32_t offset = static_cast<uint32_t>(std::max<long>(0, static_cast<long>(e.time * kRate) - static_cast<long>(at)));
        if (e.kind == Event::kOn || e.kind == Event::kOff) {
          auto n = note_event(e.kind == Event::kOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF,
                              static_cast<int16_t>(e.channel), static_cast<int16_t>(e.key), -1, e.value);
          n.header.time = offset;
          events.push(n);
        } else if (e.kind == Event::kBend) {
          const int v = std::clamp(static_cast<int>(std::lround(8192.0 + e.value * 8192.0)), 0, 16383);
          auto m = midi_event(static_cast<uint8_t>(0xe0 | e.channel), static_cast<uint8_t>(v & 0x7f),
                              static_cast<uint8_t>(v >> 7));
          m.header.time = offset;
          events.push(m);
        } else if (e.kind == Event::kMidi) {
          auto m = midi_event(e.midi[0], e.midi[1], e.midi[2]);
          m.header.time = offset;
          events.push(m);
        } else {
          clap_event_transport_t t{};
          t.header = {sizeof(t), offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
          t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
          t.tempo = e.value;
          events.push(t);
        }
      }
      runner.run(&events);
      take.left.insert(take.left.end(), runner.left().begin(), runner.left().end());
      take.right.insert(take.right.end(), runner.right().begin(), runner.right().end());
    }
  }
  plugin->destroy(plugin);
  take.left.resize(static_cast<size_t>(seconds * kRate));
  take.right.resize(static_cast<size_t>(seconds * kRate));
  return take;
}

// A single held note: on at 0, off at `hold`, rendered to `seconds`.
Take note(const Patch& patch, double hold = 1.0, double seconds = 1.5, int key = 60) {
  return render(patch, {on(0.0, key), off(hold, key)}, seconds);
}

Patch with(Patch base, std::initializer_list<Setting> extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}
Patch with(Patch base, const Patch& extra) {
  base.insert(base.end(), extra.begin(), extra.end());
  return base;
}

// The reference voice for pitch and level proofs: the morphing wavetable runs
// at the exact requested frequency (no chip timer quantization), with no attack
// or release so a measurement is not blurred by the envelope.
const Patch kClean = {{P::kWaveform, 46}, {P::kAttackMs, 0}, {P::kReleaseMs, 0}};

double midi_hz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// Pitch of a held note, measured over [from, from + length) seconds.
double pitch_at(const std::vector<float>& x, double from, double length = 0.1) {
  return measure::pitch_hz(Span(x, static_cast<size_t>(from * kRate), static_cast<size_t>(length * kRate)), kRate);
}

double rms_db(const std::vector<float>& x, double from, double length) {
  return measure::db(measure::rms(Span(x, static_cast<size_t>(from * kRate), static_cast<size_t>(length * kRate))));
}

// ----- recording --------------------------------------------------------------

struct Outcome { std::string name; std::string claim; std::vector<std::string> failures; int checks = 0; };
Outcome* g_current = nullptr;

void expect(bool ok, const std::string& what) {
  ++g_current->checks;
  if (!ok) g_current->failures.push_back(what);
}

#if defined(__GNUC__)
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string fmt(const char* f, ...) {
  char buffer[512];
  va_list args;
  va_start(args, f);
  std::vsnprintf(buffer, sizeof buffer, f, args);
  va_end(args);
  return buffer;
}

// |measured - expected| <= tolerance, reported in the given unit.
void expect_near(double measured, double expected, double tolerance, const char* unit, const std::string& what) {
  expect(std::isfinite(measured) && std::abs(measured - expected) <= tolerance,
         fmt("%s: measured %.3f %s, advertised %.3f %s (tolerance %.3f)", what.c_str(), measured, unit,
             expected, unit, tolerance));
}

struct Proof {
  const char* claim = nullptr;
  std::function<void()> run;
};

std::array<Proof, P::kParamCount>& param_proofs() {
  static std::array<Proof, P::kParamCount> table{};
  return table;
}

void prove(uint32_t id, const char* claim, std::function<void()> run) {
  auto& slot = param_proofs()[id];
  if (slot.run) {
    std::fprintf(stderr, "duplicate proof for %s\n", P::kSpecs[id].name);
    std::abort();
  }
  slot = {claim, std::move(run)};
}

}  // namespace

namespace {

// Amplitude envelope with 1 ms resolution: peak |x| over a 5 ms window centred on
// each millisecond (longer than a C-4 period, so it reads amplitude rather than
// phase). `ac` first high-passes at 30 Hz, for the YM2612, whose DAC keeps a
// resting offset after a note dies that must not read as sound.
std::vector<double> envelope_ms(const std::vector<float>& x, bool ac = false) {
  std::vector<float> y = x;
  if (ac) {
    const double r = std::exp(-2.0 * measure::kPi * 30.0 / kRate);
    double x1 = 0, y1 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
      y1 = x[i] - x1 + r * y1;
      x1 = x[i];
      y[i] = static_cast<float>(y1);
    }
  }
  const size_t hop = static_cast<size_t>(kRate / 1000.0), half = static_cast<size_t>(0.0025 * kRate);
  std::vector<double> e;
  for (size_t p = 0; p + hop <= y.size(); p += hop) {
    const size_t from = p > half ? p - half : 0;
    e.push_back(measure::peak(Span(y, from, p + half - from)));
  }
  return e;
}

// First millisecond at which the envelope reaches `fraction` of `reference`.
double time_to_reach(const std::vector<double>& env, double fraction, double reference, size_t from_ms = 0) {
  for (size_t i = from_ms; i < env.size(); ++i) if (env[i] >= fraction * reference) return static_cast<double>(i);
  return -1;
}
// First millisecond after `from_ms` at which the envelope stays below `fraction` of `reference`.
double time_to_fall(const std::vector<double>& env, double fraction, double reference, size_t from_ms) {
  for (size_t i = from_ms; i < env.size(); ++i) {
    bool below = true;
    for (size_t j = i; j < std::min(env.size(), i + 5); ++j) below = below && env[j] < fraction * reference;
    if (below) return static_cast<double>(i);
  }
  return -1;
}

// Pitch in semitones relative to MIDI note 60 every `hop` seconds. Entry i
// describes the window centred on i * hop.
std::vector<double> semitone_track(const std::vector<float>& x, double hop, double window, double fmax = 5000.0) {
  std::vector<double> out;
  const size_t lead = static_cast<size_t>(std::lround(window / 2 / hop));
  for (size_t i = 0; i < lead; ++i) out.push_back(std::nan(""));
  for (double hz : measure::pitch_track(x, kRate, hop, window, 30.0, fmax))
    out.push_back(hz > 0 ? 12.0 * std::log2(hz / midi_hz(60)) : std::nan(""));
  return out;
}

// The pitch (semitones re C4) of each step of a stepped pitch pattern, sampled in
// the middle of each step.
std::vector<double> step_pitches(const std::vector<float>& x, double step_seconds, int steps, double start = 0.0) {
  std::vector<double> out;
  for (int k = 0; k < steps; ++k) {
    const double mid = start + (k + 0.5) * step_seconds;
    const double window = std::min(0.04, step_seconds * 0.5);
    const double hz = measure::pitch_hz(Span(x, static_cast<size_t>((mid - window / 2) * kRate),
                                             static_cast<size_t>(window * kRate)), kRate, 30.0, 5000.0);
    out.push_back(hz > 0 ? 12.0 * std::log2(hz / midi_hz(60)) : std::nan(""));
  }
  return out;
}

// Times (s) at which a stepped pitch pattern changes pitch by at least half a semitone.
std::vector<double> pitch_change_times(const std::vector<float>& x, double until) {
  const double hop = 0.002;
  const auto track = semitone_track(x, hop, 0.012);
  std::vector<double> times;
  double last = std::nan("");
  for (size_t i = 0; i < track.size() && i * hop < until; ++i) {
    if (std::isnan(track[i])) continue;
    if (!std::isnan(last) && std::abs(track[i] - last) >= 0.5) times.push_back(static_cast<double>(i) * hop);
    last = track[i];
  }
  // Collapse detections within 20 ms of each other (the window straddling a step).
  std::vector<double> merged;
  for (double t : times) if (merged.empty() || t - merged.back() > 0.020) merged.push_back(t);
  return merged;
}

// Average step length: first to last change over the number of steps between
// them (unbiased by which direction a step jumps, which shifts its detection).
double mean_interval(const std::vector<double>& t) {
  return t.size() < 2 ? 0.0 : (t.back() - t.front()) / static_cast<double>(t.size() - 1);
}

void register_level_and_pitch() {
  prove(P::kGainDb, "Voice gain is a gain in dB", [] {
    // The voice runs into the output soft clipper after this gain, so the dB
    // scale is exact while the stage is linear and must keep rising to the top.
    double previous = -1e9;
    std::vector<std::pair<double, double>> levels;
    for (double g : {-36.0, -24.0, -12.0, 0.0, 6.0}) {
      const auto x = note(with(kClean, {{P::kGainDb, g}}), 0.6, 0.6).mono();
      const double level = rms_db(x, 0.2, 0.3);
      expect(level > previous, fmt("Voice gain %+.0f dB must be louder than the step below", g));
      previous = level;
      levels.push_back({g, level});
    }
    expect_near(levels[1].second - levels[0].second, 12.0, 0.3, "dB", "-36 -> -24 dB step");
    expect_near(levels[2].second - levels[1].second, 12.0, 0.5, "dB", "-24 -> -12 dB step");
  });
  prove(P::kMasterDb, "Master is an output gain in dB", [] {
    const Patch quiet = with(kClean, {{P::kGainDb, -24}});
    const double lo = rms_db(note(with(quiet, {{P::kMasterDb, -36}}), 0.6, 0.6).mono(), 0.2, 0.3);
    const double mid = rms_db(note(with(quiet, {{P::kMasterDb, -12}}), 0.6, 0.6).mono(), 0.2, 0.3);
    const double hi = rms_db(note(with(quiet, {{P::kMasterDb, 6}}), 0.6, 0.6).mono(), 0.2, 0.3);
    expect_near(mid - lo, 24.0, 0.2, "dB", "Master -36 -> -12 dB");
    expect_near(hi - mid, 18.0, 0.2, "dB", "Master -12 -> +6 dB");
  });
  prove(P::kVelocity, "Velocity On scales level by note velocity; Off ignores it", [] {
    for (double enabled : {1.0, 0.0}) {
      const Patch patch = with(kClean, {{P::kVelocity, enabled}});
      const double loud = rms_db(render(patch, {on(0, 60, 0, 1.0), off(0.6)}, 0.6).mono(), 0.2, 0.3);
      const double soft = rms_db(render(patch, {on(0, 60, 0, 0.25), off(0.6)}, 0.6).mono(), 0.2, 0.3);
      if (enabled > 0.5) expect_near(soft - loud, -12.04, 0.3, "dB", "velocity 0.25 vs 1.0 with Velocity On");
      else expect_near(soft - loud, 0.0, 0.05, "dB", "velocity 0.25 vs 1.0 with Velocity Off");
    }
  });
  prove(P::kTranspose, "Transpose shifts pitch by whole semitones", [] {
    for (double t : {-24.0, -12.0, -7.0, -1.0, 1.0, 7.0, 12.0, 24.0}) {
      const auto x = note(with(kClean, {{P::kTranspose, t}}), 0.4, 0.4).mono();
      expect_near(measure::cents(pitch_at(x, 0.15, 0.2), midi_hz(60 + t)), 0.0, 3.0, "cents",
                  fmt("Transpose %+.0f", t));
    }
  });
  prove(P::kFineTune, "Fine tune shifts pitch in cents", [] {
    for (double c : {-100.0, -50.0, 25.0, 100.0}) {
      const auto x = note(with(kClean, {{P::kFineTune, c}}), 0.4, 0.4).mono();
      expect_near(measure::cents(pitch_at(x, 0.15, 0.2), midi_hz(60)), c, 2.0, "cents", fmt("Fine tune %+.0f", c));
    }
  });
  prove(P::kPitchBendRange, "Pitch bend range is the semitones a full bend reaches", [] {
    for (double range : {0.0, 2.0, 12.0, 24.0}) {
      for (double direction : {1.0, -1.0}) {
        const Patch patch = with(kClean, {{P::kPitchBendRange, range}});
        const auto x = render(patch, {bend(0, direction), on(0.01), off(0.5)}, 0.5).mono();
        // A full MIDI bend is 8191/8192 upward and exactly -1 downward.
        const double amount = direction > 0 ? 8191.0 / 8192.0 : -1.0;
        expect_near(measure::cents(pitch_at(x, 0.2, 0.2), midi_hz(60 + range * amount)), 0.0, 3.0, "cents",
                    fmt("range %.0f, bend %+.0f", range, direction));
      }
    }
  });
  prove(P::kClockMode, "Clock selects the NTSC or PAL chip clock (and 60/50 Hz mains)", [] {
    // The NES noise channel is clocked from the CPU clock: PAL runs it at
    // 1662607/1789773 of the NTSC rate.
    auto noise_rate = [](double pal) {
      const auto x = note({{P::kWaveform, 2}, {P::kNoiseMode, 0}, {P::kNoisePeriod, 8}, {P::kClockMode, pal},
                           {P::kAttackMs, 0}, {P::kReleaseMs, 0}}, 1.0, 1.0).mono();
      size_t flips = 0;
      for (size_t i = 1; i < x.size(); ++i) flips += (x[i] > 0) != (x[i - 1] > 0);
      return static_cast<double>(flips);
    };
    expect_near(noise_rate(1.0) / noise_rate(0.0), 1662607.0 / 1789773.0, 0.02, "", "PAL/NTSC noise clock ratio");
    // Mains hum follows the clock's region.
    for (double pal : {0.0, 1.0}) {
      const auto x = note(with(kClean, {{P::kClockMode, pal}, {P::kRetroAmount, 1}, {P::kHum, 1},
                                        {P::kGainDb, -36}}), 1.0, 1.0).mono();
      const Span body(x, x.size() / 4, x.size() / 2);
      const double a50 = measure::tone_amplitude(body, kRate, 50), a60 = measure::tone_amplitude(body, kRate, 60);
      if (pal > 0.5) expect(a50 > 4 * a60, fmt("PAL hum at 50 Hz (50 Hz %.5f vs 60 Hz %.5f)", a50, a60));
      else expect(a60 > 4 * a50, fmt("NTSC hum at 60 Hz (60 Hz %.5f vs 50 Hz %.5f)", a60, a50));
    }
  });
}

void register_envelope_and_glide() {
  prove(P::kAttackMs, "Attack is the ms the note takes to rise to full level", [] {
    for (double a : {0.0, 50.0, 200.0, 500.0}) {
      const auto env = envelope_ms(note(with(kClean, {{P::kAttackMs, a}}), 0.9, 0.9).mono());
      const double full = *std::max_element(env.begin() + 700, env.begin() + 850);
      const double t100 = time_to_reach(env, 0.98, full);
      // The ramp is linear, so reaching 98% takes 0.98 of the attack.
      expect_near(t100, 0.98 * a, 2.0 + 0.03 * a, "ms", fmt("Attack %.0f ms, time to 98%%", a));
      if (a > 0) expect_near(time_to_reach(env, 0.5, full), 0.5 * a, 2.0 + 0.03 * a, "ms", fmt("Attack %.0f ms, time to 50%%", a));
    }
  });
  prove(P::kReleaseMs, "Release is the ms the note takes to fade after note-off", [] {
    // Gate-cut chips fade linearly: silence at the release time.
    for (double r : {0.0, 50.0, 300.0, 1500.0}) {
      const auto env = envelope_ms(note(with(kClean, {{P::kReleaseMs, r}}), 0.5, 0.5 + r / 1000 + 0.2).mono());
      const double held = *std::max_element(env.begin() + 400, env.begin() + 490);
      // The envelope's 5 ms window reads the fall 2.5 ms late.
      expect_near(time_to_fall(env, 0.01, held, 500) - 500.0 - 2.5, 0.99 * r, 2.0 + 0.03 * r, "ms",
                  fmt("Release %.0f ms (linear voice), time to -40 dB", r));
    }
    // Chip envelope generators (VRC7, SID) release at a constant dB rate: -60 dB at the release time.
    for (double r : {300.0, 900.0}) {
      const auto env = envelope_ms(note({{P::kWaveform, 7}, {P::kFmSustainRate, 0}, {P::kAttackMs, 0},
                                         {P::kReleaseMs, r}}, 0.5, 0.5 + r / 1000 + 0.2).mono());
      const double held = *std::max_element(env.begin() + 400, env.begin() + 490);
      expect_near(time_to_fall(env, 0.1, held, 500) - 502.5, r / 3.0, 3.0 + 0.05 * r, "ms",
                  fmt("Release %.0f ms (VRC7 envelope generator), time to -20 dB", r));
      expect_near(time_to_fall(env, 0.01, held, 500) - 502.5, 2.0 * r / 3.0, 3.0 + 0.05 * r, "ms",
                  fmt("Release %.0f ms (VRC7 envelope generator), time to -40 dB", r));
    }
  });
  prove(P::kPortamentoMs, "Portamento glides between notes with that time constant", [] {
    for (double glide : {0.0, 60.0, 250.0}) {
      const auto x = render(with(kClean, {{P::kPortamentoMs, glide}}),
                            {on(0.0, 60), off(0.5, 60), on(0.5, 72), off(2.0, 72)}, 2.0).mono();
      const auto track = semitone_track(x, 0.001, 0.008);
      // Exponential glide: 63.2% of the 12-semitone interval after one time constant.
      double t63 = -1;
      for (size_t i = 500; i < track.size(); ++i)
        if (!std::isnan(track[i]) && track[i] >= 12.0 * (1.0 - std::exp(-1.0))) { t63 = static_cast<double>(i) - 500.0; break; }
      expect_near(t63, glide, 6.0 + 0.08 * glide, "ms", fmt("Portamento %.0f ms, time to 63%% of the interval", glide));
      // And it follows the exponential all the way: 99.3% of the way after five time constants.
      const size_t t5 = 500 + static_cast<size_t>(std::max(20.0, 5.0 * glide));
      expect_near(track[t5], glide > 0 ? 12.0 * (1.0 - std::exp(-5.0)) : 12.0, 0.1, "semitones",
                  fmt("Portamento %.0f ms after five time constants", glide));
    }
  });
  prove(P::kHardwareEnvelope, "Hardware envelope steps the level from 15 to 0 on the chip's clock", [] {
    const auto held = envelope_ms(note(with(kClean, {{P::kHardwareEnvelope, 0}}), 1.2, 1.2).mono());
    expect(held[1100] > 0.9 * held[100], "Off: the note holds its level");
    const auto env = envelope_ms(note(with(kClean, {{P::kHardwareEnvelope, 1}, {P::kEnvelopeRate, 8}}), 1.2, 1.2).mono());
    const double full = env[5];
    // 15 steps of (16 - rate) ticks at 240 Hz: rate 8 is gone at 500 ms.
    expect_near(time_to_fall(env, 0.02, full, 0), 15.0 * 8.0 / 240.0 * 1000.0, 6.0, "ms", "On, rate 8: silent after 15 steps");
    // It is a staircase: the level at the middle of step k is (15 - k)/15.
    for (int k : {1, 5, 10}) {
      const size_t mid = static_cast<size_t>((k + 0.5) * 8.0 / 240.0 * 1000.0);
      expect_near(env[mid] / full, (15.0 - k) / 15.0, 0.03, "", fmt("On, rate 8: level of step %d", k));
    }
  });
  prove(P::kEnvelopeRate, "Envelope rate sets the hardware envelope's step period", [] {
    for (double rate : {0.0, 8.0, 12.0, 15.0}) {
      const auto env = envelope_ms(note(with(kClean, {{P::kHardwareEnvelope, 1}, {P::kEnvelopeRate, rate}}), 1.2, 1.2).mono());
      expect_near(time_to_fall(env, 0.02, env[3], 0), 15.0 * (16.0 - rate) / 240.0 * 1000.0, 6.0, "ms",
                  fmt("Envelope rate %.0f: time to silence", rate));
    }
  });
  prove(P::kSweepDepth, "Sweep depth is the semitones the pitch sweeps", [] {
    for (double depth : {-12.0, -5.0, 7.0, 24.0}) {
      const auto x = note(with(kClean, {{P::kSweepDepth, depth}, {P::kSweepTime, 100}}), 0.5, 0.5).mono();
      expect_near(measure::cents(pitch_at(x, 0.3, 0.15), midi_hz(60 + depth)), 0.0, 3.0, "cents",
                  fmt("Sweep depth %+.0f: pitch after the sweep", depth));
    }
  });
  prove(P::kSweepTime, "Sweep time is the ms the sweep takes", [] {
    for (double time : {100.0, 400.0}) {
      const auto x = note(with(kClean, {{P::kSweepDepth, 12}, {P::kSweepTime, time}}), 0.8, 0.8).mono();
      const auto track = semitone_track(x, 0.001, 0.008);
      // A linear ramp in semitones: halfway at half the time, done at the time.
      expect_near(track[static_cast<size_t>(time / 2)], 6.0, 0.25, "semitones", fmt("Sweep %.0f ms: pitch at half time", time));
      expect_near(track[static_cast<size_t>(time + 20)], 12.0, 0.1, "semitones", fmt("Sweep %.0f ms: pitch just after", time));
    }
  });
  prove(P::kVibratoRate, "Vibrato rate is the vibrato frequency in Hz", [] {
    for (double rate : {2.0, 5.5, 12.0}) {
      const auto x = note(with(kClean, {{P::kVibratoDepth, 0.5}, {P::kVibratoRate, rate}}), 2.0, 2.0).mono();
      const auto hz = measure::pitch_track(x, kRate, 0.002, 0.012);
      expect_near(measure::modulation_hz(hz, 0.002, 1.0, 30.0), rate, 0.05 * rate, "Hz", fmt("Vibrato rate %.1f Hz", rate));
    }
  });
  prove(P::kVibratoDepth, "Vibrato depth is the peak deviation in semitones", [] {
    for (double depth : {0.0, 0.25, 1.0, 2.0}) {
      const auto track = semitone_track(note(with(kClean, {{P::kVibratoDepth, depth}, {P::kVibratoRate, 4}}), 1.5, 1.5).mono(),
                                        0.002, 0.010);
      double lo = 1e9, hi = -1e9;
      for (size_t i = 100; i < track.size() - 10; ++i) if (!std::isnan(track[i])) { lo = std::min(lo, track[i]); hi = std::max(hi, track[i]); }
      expect_near((hi - lo) / 2.0, depth, 0.03 + 0.05 * depth, "semitones", fmt("Vibrato depth %.2f", depth));
    }
  });
  prove(P::kVibratoDelay, "Vibrato delay holds the pitch steady for that many ms", [] {
    for (double delay : {0.0, 250.0, 800.0}) {
      const auto track = semitone_track(note(with(kClean, {{P::kVibratoDepth, 1.0}, {P::kVibratoRate, 5}, {P::kVibratoDelay, delay}}), 1.5, 1.5).mono(),
                                        0.001, 0.008);
      double onset = -1;
      for (size_t i = 5; i < track.size(); ++i) if (!std::isnan(track[i]) && std::abs(track[i]) > 0.15) { onset = static_cast<double>(i); break; }
      // Starting from zero phase, the pitch passes 0.15 semitone asin(0.15)/(2 pi 5 Hz) = 4.8 ms after the delay.
      expect_near(onset, delay + 4.8, 4.0, "ms", fmt("Vibrato delay %.0f ms: onset of the wobble", delay));
    }
  });
}

}  // namespace

namespace {

// A stepped pitch pattern's steps, in semitones re C4, `steps` of `step` seconds.
std::vector<double> pattern(const Patch& patch, double step, int steps, int key = 60) {
  return step_pitches(render(patch, {on(0, key), off(step * steps + 0.05, key)}, step * steps + 0.05).mono(), step, steps);
}

void expect_pattern(const std::vector<double>& measured, const std::vector<double>& expected, double tolerance,
                    const std::string& what) {
  for (size_t k = 0; k < expected.size() && k < measured.size(); ++k)
    expect_near(measured[k], expected[k], tolerance, "semitones", fmt("%s, step %zu", what.c_str(), k + 1));
}

// Pulse duty of each step of a duty pattern on the NES pulse at C-3.
std::vector<double> duty_pattern(const Patch& patch, double step, int steps) {
  const auto x = render(with({{P::kWaveform, 0}, {P::kAttackMs, 0}, {P::kReleaseMs, 0}}, patch),
                        {on(0, 48), off(step * steps + 0.05, 48)}, step * steps + 0.05).mono();
  std::vector<double> out;
  for (int k = 0; k < steps; ++k)
    out.push_back(measure::duty(Span(x, static_cast<size_t>((k + 0.2) * step * kRate), static_cast<size_t>(0.6 * step * kRate))));
  return out;
}

constexpr double kDutyFraction[] = {0.125, 0.25, 0.5, 0.75};
constexpr double kStepsPerBeat[] = {0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0};

void register_sequences() {
  prove(P::kArpMode, "Arpeggio plays its named interval pattern", [] {
    const std::vector<std::vector<double>> intervals = {{0, 0, 0}, {0, 4, 7}, {0, 3, 7}, {0, 12, 24}, {0, 3, 8}};
    const char* names[] = {"Off", "Major", "Minor", "Octaves", "NES chord"};
    for (int mode = 0; mode <= 4; ++mode) {
      const auto steps = pattern(with(kClean, {{P::kArpMode, static_cast<double>(mode)}, {P::kArpRate, 10}}), 0.1, 6);
      std::vector<double> expected;
      for (int k = 0; k < 6; ++k) expected.push_back(intervals[static_cast<size_t>(mode)][static_cast<size_t>(k % 3)]);
      expect_pattern(steps, expected, 0.05, names[mode]);
    }
    // User steps plays the Step 1..N lane (defaults 0, +4, +7, +12 over 4 steps).
    expect_pattern(pattern(with(kClean, {{P::kArpMode, 5}, {P::kArpRate, 10}}), 0.1, 8), {0, 4, 7, 12, 0, 4, 7, 12}, 0.05,
                   "User steps");
  });
  prove(P::kArpRate, "Arpeggio rate is the number of steps per second", [] {
    for (double rate : {4.0, 12.0, 30.0}) {
      const auto x = note(with(kClean, {{P::kArpMode, 3}, {P::kArpRate, rate}}), 1.2, 1.2).mono();
      expect_near(mean_interval(pitch_change_times(x, 1.2)), 1.0 / rate, 0.002 + 0.02 / rate, "s",
                  fmt("Arpeggio rate %.0f steps/s: step length", rate));
    }
  });
  prove(P::kSequenceLength, "Sequence length is the number of user steps before it repeats", [] {
    const Patch steps = with(kClean, {{P::kArpMode, 5}, {P::kArpRate, 10}, {P::kSequence1, 0}, {P::kSequence2, 2},
                                      {P::kSequence3, 4}, {P::kSequence4, 6}, {P::kSequence5, 8}, {P::kSequence6, 10},
                                      {P::kSequence7, 12}, {P::kSequence8, 14}});
    for (int length : {1, 3, 8}) {
      std::vector<double> expected;
      for (int k = 0; k < 9; ++k) expected.push_back(2.0 * (k % length));
      expect_pattern(pattern(with(steps, {{P::kSequenceLength, static_cast<double>(length)}}), 0.1, 9), expected, 0.05,
                     fmt("Sequence length %d", length));
    }
  });
  for (int k = 0; k < 8; ++k) {
    prove(P::kSequence1 + static_cast<uint32_t>(k), "User step is that step's offset in semitones", [k] {
      for (double value : {-24.0, -5.0, 11.0, 24.0}) {
        Patch patch = with(kClean, {{P::kArpMode, 5}, {P::kArpRate, 10}, {P::kSequenceLength, 8}});
        for (int j = 0; j < 8; ++j) patch.push_back({P::kSequence1 + static_cast<uint32_t>(j), j == k ? value : 0.0});
        const auto steps = pattern(patch, 0.1, 8);
        expect_near(steps[static_cast<size_t>(k)], value, 0.05, "semitones", fmt("Step %d = %+.0f", k + 1, value));
        expect_near(steps[static_cast<size_t>((k + 1) % 8)], 0.0, 0.05, "semitones", fmt("Step %d leaves its neighbour", k + 1));
      }
    });
  }
  prove(P::kTempoSync, "Tempo sync locks sequence steps to the host tempo", [] {
    const Patch arp = with(kClean, {{P::kArpMode, 3}, {P::kArpRate, 12}, {P::kSyncDivision, 2}});
    for (double sync : {0.0, 1.0}) {
      const auto x = render(with(arp, {{P::kTempoSync, sync}}), {tempo(0, 150), on(0.001), off(2.0)}, 2.0).mono();
      // Off: 12 steps/s free-running. On: 1 step per beat at 150 BPM = 2.5 steps/s.
      expect_near(mean_interval(pitch_change_times(x, 2.0)), sync > 0.5 ? 0.4 : 1.0 / 12.0, 0.005, "s",
                  fmt("Tempo sync %s: step length", sync > 0.5 ? "On at 150 BPM, 1 per beat" : "Off, 12 steps/s"));
    }
  });
  prove(P::kSyncDivision, "Sync division sets steps per beat, and the synced echo's length", [] {
    for (int division = 0; division < 8; ++division) {
      const double bpm = division < 2 ? 240.0 : 120.0;
      const double step = 60.0 / bpm / kStepsPerBeat[division];
      const double length = std::max(1.0, 5.0 * step);
      const auto x = render(with(kClean, {{P::kArpMode, 3}, {P::kTempoSync, 1}, {P::kSyncDivision, static_cast<double>(division)}}),
                            {tempo(0, bpm), on(0.001), off(length)}, length).mono();
      expect_near(mean_interval(pitch_change_times(x, length)), step, 0.003 + 0.01 * step, "s",
                  fmt("%s at %.0f BPM: arpeggio step", P::kSyncDivisionNames[division], bpm));
    }
    // With Tempo sync on, Echo time follows the same division: one repeat per step.
    for (int division : {2, 3, 5}) {
      const double bpm = 120.0, expected = 60.0 / bpm / kStepsPerBeat[division];
      const auto x = render(with(kClean, {{P::kTempoSync, 1}, {P::kSyncDivision, static_cast<double>(division)},
                                          {P::kEchoMix, 0.5}, {P::kEchoFeedback, 0}}),
                            {tempo(0, bpm), on(0.001), off(0.03)}, 1.0).mono();
      const auto env = envelope_ms(x);
      const double repeat = time_to_reach(env, 0.3, env[15], 60) - 1.0;
      expect_near(repeat / 1000.0, expected, 0.004, "s", fmt("%s at %.0f BPM: synced echo delay", P::kSyncDivisionNames[division], bpm));
    }
  });
  prove(P::kDutySeqMode, "Duty sequence Off holds Pulse duty, Loop cycles the steps, One shot holds the last", [] {
    const Patch steps = {{P::kDuty, 2}, {P::kDutySeqLength, 3}, {P::kDutySeqRate, 10},
                         {P::kDutyStep1, 0}, {P::kDutyStep2, 1}, {P::kDutyStep3, 3}};
    const auto off_duty = duty_pattern(with(steps, {{P::kDutySeqMode, 0}}), 0.1, 6);
    const auto loop = duty_pattern(with(steps, {{P::kDutySeqMode, 1}}), 0.1, 6);
    const auto shot = duty_pattern(with(steps, {{P::kDutySeqMode, 2}}), 0.1, 6);
    const int looped[] = {0, 1, 3, 0, 1, 3}, held[] = {0, 1, 3, 3, 3, 3};
    for (size_t k = 0; k < 6; ++k) {
      expect_near(off_duty[k], 0.5, 0.02, "", fmt("Off, step %zu holds Pulse duty 50%%", k + 1));
      expect_near(loop[k], kDutyFraction[looped[k]], 0.02, "", fmt("Loop, step %zu", k + 1));
      expect_near(shot[k], kDutyFraction[held[k]], 0.02, "", fmt("One shot, step %zu", k + 1));
    }
  });
  prove(P::kDutySeqLength, "Duty length is the number of duty steps before it repeats", [] {
    for (int length : {2, 5}) {
      Patch patch = {{P::kDutySeqMode, 1}, {P::kDutySeqRate, 10}, {P::kDutySeqLength, static_cast<double>(length)}};
      for (int j = 0; j < 8; ++j) patch.push_back({P::kDutyStep1 + static_cast<uint32_t>(j), static_cast<double>(j % 4)});
      const auto d = duty_pattern(patch, 0.1, 8);
      for (size_t k = 0; k < 8; ++k)
        expect_near(d[k], kDutyFraction[(k % static_cast<size_t>(length)) % 4], 0.02, "", fmt("Duty length %d, step %zu", length, k + 1));
    }
  });
  prove(P::kDutySeqRate, "Duty step rate is the number of duty steps per second", [] {
    for (double rate : {5.0, 20.0}) {
      const auto d = duty_pattern({{P::kDutySeqMode, 1}, {P::kDutySeqLength, 2}, {P::kDutySeqRate, rate},
                                   {P::kDutyStep1, 0}, {P::kDutyStep2, 2}}, 1.0 / rate, 6);
      for (size_t k = 0; k < 6; ++k) expect_near(d[k], k % 2 ? 0.5 : 0.125, 0.02, "", fmt("Duty step rate %.0f, step %zu", rate, k + 1));
    }
  });
  for (int k = 0; k < 8; ++k) {
    prove(P::kDutyStep1 + static_cast<uint32_t>(k), "Duty step is the pulse duty played on that step", [k] {
      for (int value = 0; value < 4; ++value) {
        Patch patch = {{P::kDutySeqMode, 1}, {P::kDutySeqLength, 8}, {P::kDutySeqRate, 10}};
        for (int j = 0; j < 8; ++j) patch.push_back({P::kDutyStep1 + static_cast<uint32_t>(j), j == k ? static_cast<double>(value) : (value == 2 ? 0.0 : 2.0)});
        expect_near(duty_pattern(patch, 0.1, 8)[static_cast<size_t>(k)], kDutyFraction[value], 0.02, "",
                    fmt("Duty step %d = %s", k + 1, P::kDutyNames[value]));
      }
    });
  }
  prove(P::kCentsSeqMode, "Cents sequence Off holds pitch, Loop cycles the steps, One shot holds the last", [] {
    const Patch steps = with(kClean, {{P::kCentsSeqLength, 3}, {P::kCentsSeqRate, 5},
                                      {P::kCentsStep1, -50}, {P::kCentsStep2, 30}, {P::kCentsStep3, 100}});
    const double looped[] = {-50, 30, 100, -50, 30, 100}, held[] = {-50, 30, 100, 100, 100, 100};
    const auto off_steps = pattern(with(steps, {{P::kCentsSeqMode, 0}}), 0.2, 6);
    const auto loop = pattern(with(steps, {{P::kCentsSeqMode, 1}}), 0.2, 6);
    const auto shot = pattern(with(steps, {{P::kCentsSeqMode, 2}}), 0.2, 6);
    for (size_t k = 0; k < 6; ++k) {
      expect_near(off_steps[k] * 100, 0, 2, "cents", fmt("Off, step %zu", k + 1));
      expect_near(loop[k] * 100, looped[k], 2, "cents", fmt("Loop, step %zu", k + 1));
      expect_near(shot[k] * 100, held[k], 2, "cents", fmt("One shot, step %zu", k + 1));
    }
  });
  prove(P::kCentsSeqLength, "Cents length is the number of cents steps before it repeats", [] {
    Patch base = with(kClean, {{P::kCentsSeqMode, 1}, {P::kCentsSeqRate, 5}});
    for (int j = 0; j < 8; ++j) base.push_back({P::kCentsStep1 + static_cast<uint32_t>(j), 20.0 * j});
    for (int length : {2, 5}) {
      const auto steps = pattern(with(base, {{P::kCentsSeqLength, static_cast<double>(length)}}), 0.2, 7);
      for (size_t k = 0; k < 7; ++k)
        expect_near(steps[k] * 100, 20.0 * static_cast<double>(k % static_cast<size_t>(length)), 2, "cents", fmt("Cents length %d, step %zu", length, k + 1));
    }
  });
  prove(P::kCentsSeqRate, "Cents step rate is the number of cents steps per second", [] {
    for (double rate : {4.0, 16.0}) {
      const auto steps = pattern(with(kClean, {{P::kCentsSeqMode, 1}, {P::kCentsSeqLength, 2}, {P::kCentsSeqRate, rate},
                                               {P::kCentsStep1, 0}, {P::kCentsStep2, 100}}), 1.0 / rate, 6);
      for (size_t k = 0; k < 6; ++k) expect_near(steps[k] * 100, k % 2 ? 100 : 0, 2, "cents", fmt("Cents step rate %.0f, step %zu", rate, k + 1));
    }
  });
  for (int k = 0; k < 8; ++k) {
    prove(P::kCentsStep1 + static_cast<uint32_t>(k), "Cents step is that step's offset in cents", [k] {
      for (double value : {-100.0, -35.0, 60.0, 100.0}) {
        Patch patch = with(kClean, {{P::kCentsSeqMode, 1}, {P::kCentsSeqLength, 8}, {P::kCentsSeqRate, 8}});
        for (int j = 0; j < 8; ++j) patch.push_back({P::kCentsStep1 + static_cast<uint32_t>(j), j == k ? value : 0.0});
        expect_near(pattern(patch, 0.125, 8)[static_cast<size_t>(k)] * 100, value, 2, "cents", fmt("Cents step %d = %+.0f", k + 1, value));
      }
    });
  }
}

}  // namespace

namespace {

// Harmonic amplitudes (dB re the fundamental) of a held note's steady body.
std::vector<double> harmonic_db(const std::vector<float>& x, double f0, int count, double from = 0.2, double length = 0.4) {
  const auto h = measure::harmonics(Span(x, static_cast<size_t>(from * kRate), static_cast<size_t>(length * kRate)), kRate, f0, count);
  std::vector<double> out;
  for (double a : h) out.push_back(measure::db(a / std::max(h[0], 1e-12)));
  return out;
}

// Correlation of two log power spectra: 1 means the same timbre.
double timbre_similarity(const std::vector<float>& a, const std::vector<float>& b) {
  auto logs = [](const std::vector<float>& x) {
    auto p = measure::power_spectrum(Span(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.5 * kRate)), 12);
    std::vector<float> l;
    const double top = *std::max_element(p.begin(), p.end());
    for (size_t k = 1; k < p.size() / 3; ++k) l.push_back(static_cast<float>(std::max(-80.0, 10 * std::log10(p[k] / top + 1e-30))));
    return l;
  };
  const auto la = logs(a), lb = logs(b);
  return measure::correlation(Span(la), Span(lb));
}

// Samples per LFSR run that repeats: normalized autocorrelation at `lag`.
double repeats_at(const std::vector<float>& x, double lag_seconds) {
  return measure::autocorrelation(Span(x, static_cast<size_t>(0.1 * kRate), static_cast<size_t>(0.6 * kRate)),
                                  static_cast<size_t>(std::lround(lag_seconds * kRate)));
}

const Patch kQuiet = {{P::kGainDb, -24}};  // keep the output stage linear for spectral proofs

void register_oscillators() {
  prove(P::kDuty, "Pulse duty is the pulse's high time: 12.5/25/50/75%", [] {
    for (int voice : {0, 10}) {
      for (int step = 0; step < 4; ++step) {
        const auto x = note({{P::kWaveform, static_cast<double>(voice)}, {P::kDuty, static_cast<double>(step)},
                             {P::kHardwareEnvelope, 0}, {P::kAttackMs, 0}}, 0.6, 0.6, 36).mono();
        expect_near(measure::duty(Span(x, static_cast<size_t>(0.1 * kRate), static_cast<size_t>(0.4 * kRate))),
                    kDutyFraction[step], 0.01, "", fmt("%s at %s", P::kWaveNames[voice], P::kDutyNames[step]));
      }
    }
  });
  prove(P::kNoisePeriod, "Noise period selects the NES noise clock from the period table", [] {
    // NTSC 2A03 noise periods in CPU cycles; the 15-bit shift register (feedback
    // bit 0 xor bit 1, starting from 1 as at power-up) clocks once per period.
    constexpr double table[] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
    auto reference_flips = [](size_t clocks) {
      uint32_t lfsr = 1;
      size_t flips = 0;
      int last = -1;
      for (size_t i = 0; i < clocks; ++i) {
        const uint32_t feedback = (lfsr ^ (lfsr >> 1)) & 1U;
        lfsr = (lfsr >> 1) | (feedback << 14);
        const int bit = static_cast<int>(lfsr & 1U);
        if (last >= 0 && bit != last) ++flips;
        last = bit;
      }
      return static_cast<double>(flips);
    };
    for (int period : {6, 9, 12, 15}) {
      const double seconds = 3.0;
      const auto x = note({{P::kWaveform, 2}, {P::kNoiseMode, 0}, {P::kNoisePeriod, static_cast<double>(period)},
                           {P::kAttackMs, 0}}, seconds, seconds).mono();
      size_t flips = 0;
      for (size_t i = 1; i < x.size(); ++i) flips += (x[i] > 0) != (x[i - 1] > 0);
      const double clock = 1789773.0 / table[period];
      expect_near(static_cast<double>(flips) / reference_flips(static_cast<size_t>(clock * seconds)), 1.0, 0.02, "",
                  fmt("Noise period %d: shift clock %.0f Hz (transitions vs the 2A03 sequence)", period, clock));
    }
  });
  prove(P::kNoiseMode, "Noise mode switches the LFSR to its short, repeating sequence", [] {
    // NES short mode repeats every 93 clocks; long mode (32767) does not repeat within the window.
    const double nes_clock = 1789773.0 / 1016.0;  // period 13
    for (double mode : {0.0, 1.0}) {
      const auto x = note({{P::kWaveform, 2}, {P::kNoiseMode, mode}, {P::kNoisePeriod, 13}, {P::kAttackMs, 0}}, 0.8, 0.8).mono();
      const double r = repeats_at(x, 93.0 / nes_clock);
      if (mode > 0.5) expect(r > 0.9, fmt("NES short mode repeats every 93 clocks (r=%.3f)", r));
      else expect(r < 0.3, fmt("NES long mode does not repeat at 93 clocks (r=%.3f)", r));
    }
    // Game Boy: 7-bit mode repeats every 127 clocks; C-4 clocks the register at 4096 Hz.
    for (double mode : {0.0, 1.0}) {
      const auto x = note({{P::kWaveform, 12}, {P::kNoiseMode, mode}, {P::kHardwareEnvelope, 0}, {P::kAttackMs, 0}}, 0.8, 0.8).mono();
      const double r = repeats_at(x, 127.0 / 4096.0);
      if (mode > 0.5) expect(r > 0.9, fmt("Game Boy 7-bit mode repeats every 127 clocks (r=%.3f)", r));
      else expect(r < 0.3, fmt("Game Boy 15-bit mode does not repeat at 127 clocks (r=%.3f)", r));
    }
    // SN76489: periodic mode (Off) is a 16-step pulse at the tone rate, white mode is noise.
    for (double mode : {0.0, 1.0}) {
      const auto x = note({{P::kWaveform, 14}, {P::kNoiseMode, mode}, {P::kAttackMs, 0}}, 0.8, 0.8, 84).mono();
      const double hz = pitch_at(x, 0.2, 0.3);
      if (mode > 0.5) expect(hz == 0.0, fmt("SMS white noise is aperiodic (pitch %.1f Hz)", hz));
      else expect(hz > 0.0, "SMS periodic noise has a pitch");
    }
  });
  prove(P::kExpansionShape, "Shape selects each chip's duty, accumulator, table or waveform", [] {
    // VRC6 pulse: eight duties from 1/16 to 8/16.
    for (int shape = 0; shape < 8; ++shape) {
      const auto x = note(with(kQuiet, {{P::kWaveform, 3}, {P::kExpansionShape, static_cast<double>(shape)}, {P::kAttackMs, 0}}), 0.6, 0.6, 36).mono();
      expect_near(measure::duty(Span(x, static_cast<size_t>(0.1 * kRate), static_cast<size_t>(0.4 * kRate))), (shape + 1) / 16.0, 0.01, "",
                  fmt("VRC6 pulse shape %d duty", shape));
    }
    // SID (filter open): 0 saw (every harmonic, falling), 1 triangle (odd harmonics
    // only, falling faster), 2 pulse at the Pulse duty, 3-6 the combined waveforms,
    // 7 noise.
    const Patch sid = with(kQuiet, {{P::kWaveform, 39}, {P::kChipCutoff, 16000}, {P::kReleaseMs, 0}, {P::kDuty, 1}});
    const double f0 = midi_hz(48);
    auto sid_note = [&](int shape) { return note(with(sid, {{P::kExpansionShape, static_cast<double>(shape)}}), 0.6, 0.6, 48).mono(); };
    const auto saw = harmonic_db(sid_note(0), f0, 4), tri = harmonic_db(sid_note(1), f0, 4);
    expect(saw[1] > -10 && saw[2] > -14 && saw[1] > saw[2] && saw[2] > saw[3], fmt("SID shape 0 is a saw (h2 %.1f, h3 %.1f dB)", saw[1], saw[2]));
    expect(tri[1] < -30 && tri[3] < -30 && tri[2] < saw[2] - 6, fmt("SID shape 1 is a triangle (h2 %.1f, h3 %.1f dB)", tri[1], tri[2]));
    expect_near(measure::duty(Span(sid_note(2), static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.3 * kRate))), 0.25, 0.03, "",
                "SID shape 2 is a pulse at the Pulse duty (25%)");
    std::vector<std::vector<float>> combined;
    for (int shape = 3; shape <= 6; ++shape) combined.push_back(sid_note(shape));
    for (size_t a = 0; a < combined.size(); ++a)
      for (size_t b = a + 1; b < combined.size(); ++b)
        expect(measure::correlation(Span(combined[a], 9600, 4800), Span(combined[b], 9600, 4800)) < 0.98 ||
                   std::abs(rms_db(combined[a], 0.2, 0.3) - rms_db(combined[b], 0.2, 0.3)) > 1.0,
               fmt("SID combined shapes %zu and %zu differ in shape or level", a + 3, b + 3));
    expect(pitch_at(sid_note(7), 0.2, 0.2) == 0.0, "SID shape 7 is noise");
    // VRC6 saw: the accumulator rate (shape x 8 + 7) is the saw's volume until it
    // passes 42 and overflows, folding the ramp.
    std::vector<double> saw_level;
    std::vector<std::vector<float>> saws;
    for (int shape = 0; shape < 8; ++shape) {
      saws.push_back(note(with(kQuiet, {{P::kWaveform, 4}, {P::kExpansionShape, static_cast<double>(shape)}, {P::kAttackMs, 0}}), 0.6, 0.6).mono());
      saw_level.push_back(rms_db(saws.back(), 0.2, 0.3));
    }
    for (int shape = 1; shape <= 4; ++shape)
      expect_near(saw_level[static_cast<size_t>(shape)] - saw_level[0], measure::db((shape * 8.0 + 7.0) / 7.0), 1.0, "dB",
                  fmt("VRC6 saw shape %d: accumulator rate %d sets the level", shape, shape * 8 + 7));
    for (int shape = 5; shape < 8; ++shape)
      expect(measure::correlation(Span(saws[static_cast<size_t>(shape)], 9600, 4800), Span(saws[0], 9600, 4800)) < 0.95,
             fmt("VRC6 saw shape %d overflows the accumulator and folds the ramp", shape));
    // Wavetable chips: each shape is its own table, at the same pitch, moving
    // through the chip's documented progression; 7 is the plain reset ramp.
    const double c4 = midi_hz(60);
    auto table_note = [&](int voice, int shape) {
      return note(with(kQuiet, {{P::kWaveform, static_cast<double>(voice)}, {P::kExpansionShape, static_cast<double>(shape)},
                                {P::kAttackMs, 0}}), 0.6, 0.6).mono();
    };
    for (int voice : {5, 6, 11, 26, 40})
      for (int shape = 0; shape < 8; ++shape)
        expect_near(measure::cents(pitch_at(table_note(voice, shape), 0.2, 0.2), c4), 0, 15, "cents",
                    fmt("%s shape %d plays C-4", P::kWaveNames[voice], shape));
    // FDS: a sine with a 2nd harmonic of (shape - 3.5)/14, through the chip's 2 kHz RC.
    for (int shape : {0, 2, 5, 6}) {
      const double rc = 1.0 / std::sqrt(1.0 + std::pow(2 * c4 / 2000.0, 2)) * std::sqrt(1.0 + std::pow(c4 / 2000.0, 2));
      expect_near(harmonic_db(table_note(5, shape), c4, 2)[1], measure::db(std::abs(shape - 3.5) / 14.0 * rc), 1.5, "dB",
                  fmt("FDS shape %d: 2nd harmonic", shape));
    }
    // N163: shape s is a table of 4(s + 1) steps; a staircase of L steps images at harmonic L - 1.
    for (int shape : {0, 1, 3}) {
      const int steps = 4 * (shape + 1);
      const auto h = harmonic_db(table_note(6, shape), c4, steps + 1);
      expect(h[static_cast<size_t>(steps - 2)] > -45, fmt("N163 shape %d: %d-step table images at harmonic %d (%.1f dB)", shape, steps, steps - 1,
                                                          h[static_cast<size_t>(steps - 2)]));
    }
    // PC Engine: sine blending into a ramp; SCC: sine blending into its 2nd harmonic.
    for (int voice : {26, 40}) {
      const auto sine_end = table_note(voice, 0);
      double previous = 1.0;
      for (int shape : {2, 4, 6}) {
        const double similarity = measure::correlation(Span(table_note(voice, shape), 9600, 4800), Span(sine_end, 9600, 4800));
        expect(similarity < previous - 0.01, fmt("%s shape %d: blends further from shape 0 (correlation %.3f)", P::kWaveNames[voice], shape, similarity));
        previous = similarity;
      }
    }
    // Game Boy wave: every shape must be its own table.
    std::vector<std::vector<float>> gb;
    for (int shape = 0; shape < 8; ++shape) gb.push_back(table_note(11, shape));
    for (size_t a = 0; a < gb.size(); ++a)
      for (size_t b = a + 1; b < gb.size(); ++b)
        expect(measure::correlation(Span(gb[a], 9600, 4800), Span(gb[b], 9600, 4800)) < 0.995,
               fmt("Game Boy wave shapes %zu and %zu are different tables", a, b));
    // POKEY and TIA: shape 0 is a pure tone, the others are polynomial noise.
    for (int voice : {24, 44}) {
      expect(pitch_at(note({{P::kWaveform, static_cast<double>(voice)}, {P::kExpansionShape, 0}}, 0.6, 0.6).mono(), 0.2, 0.2) > 0,
             fmt("%s shape 0 is a tone", P::kWaveNames[voice]));
      const auto noisy = note({{P::kWaveform, static_cast<double>(voice)}, {P::kExpansionShape, 1}}, 0.6, 0.6).mono();
      const auto tone = note({{P::kWaveform, static_cast<double>(voice)}, {P::kExpansionShape, 0}}, 0.6, 0.6).mono();
      expect(timbre_similarity(noisy, tone) < 0.9, fmt("%s shape 1 is not the pure tone", P::kWaveNames[voice]));
    }
  });
  prove(P::kStrictHardware, "Strict hardware enforces the chip's limits: one note per stack channel, raw pulses", [] {
    // A stack channel is one hardware voice: with Strict on, a second note on it replaces the first.
    for (double strict : {0.0, 1.0}) {
      const auto x = render({{P::kWaveform, 41}, {P::kStrictHardware, strict}, {P::kAttackMs, 0}},
                            {on(0, 60), on(0.2, 67), off(0.6, 60), off(0.6, 67)}, 0.6).mono();
      const Span body(x, static_cast<size_t>(0.3 * kRate), static_cast<size_t>(0.25 * kRate));
      const double c4 = measure::tone_amplitude(body, kRate, midi_hz(60)), g4 = measure::tone_amplitude(body, kRate, midi_hz(67));
      if (strict > 0.5) expect(c4 < 0.05 * g4, fmt("Strict: the second note replaced the first (C4 %.4f, G4 %.4f)", c4, g4));
      else expect(c4 > 0.3 * g4, fmt("Not strict: both notes sound (C4 %.4f, G4 %.4f)", c4, g4));
    }
    // The NES pulse drops its band-limiting: a high note aliases, so energy appears off the harmonic series.
    auto off_harmonic = [](double strict) {
      const auto x = note(with(kQuiet, {{P::kWaveform, 0}, {P::kDuty, 2}, {P::kStrictHardware, strict}}), 0.6, 0.6, 96).mono();
      const Span body(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.3 * kRate));
      const auto p = measure::power_spectrum(body, 13);
      const double f0 = 1789773.0 / (16.0 * (std::round(1789773.0 / (16.0 * midi_hz(96)) - 1.0) + 1.0));
      double on = 0, all = 0;
      for (size_t k = 1; k < p.size(); ++k) {
        const double hz = static_cast<double>(k) * kRate / static_cast<double>(p.size() * 2);
        const double h = hz / f0;
        all += p[k];
        if (std::abs(h - std::round(h)) * f0 < 40.0) on += p[k];
      }
      return 1.0 - on / all;
    };
    const double smooth = off_harmonic(0.0), raw = off_harmonic(1.0);
    expect(raw > 3.0 * smooth, fmt("Strict NES pulse aliases (off-harmonic power %.5f vs band-limited %.5f)", raw, smooth));
  });
  prove(P::kFmRatio, "FM ratio is the modulator frequency as a multiple of the carrier", [] {
    // sin(p + I sin(r p)) has partials at |1 + k r| f0: a harmonic that is a
    // multiple of r (other than 1 +/- ...) is absent, the next one up is present.
    for (double ratio : {2.0, 3.0, 4.0}) {
      const auto h = harmonic_db(note(with(kQuiet, {{P::kWaveform, 7}, {P::kFmRatio, ratio}, {P::kFmIndex, 2}, {P::kFmSustainRate, 0}}), 0.6, 0.6).mono(),
                                 midi_hz(60), 6);
      expect(h[static_cast<size_t>(ratio) - 1] < -35, fmt("ratio %.0f: harmonic %.0f absent (%.1f dB)", ratio, ratio, h[static_cast<size_t>(ratio) - 1]));
      expect(h[static_cast<size_t>(ratio)] > -25, fmt("ratio %.0f: harmonic %.0f present (%.1f dB)", ratio, ratio + 1, h[static_cast<size_t>(ratio)]));
    }
  });
  prove(P::kFmIndex, "FM index is the modulation index (Bessel sidebands)", [] {
    // Ratio 4: the carrier is J0(I), the first upper sideband (5th harmonic) J1(I).
    for (double index : {0.0, 0.5, 1.0, 2.0}) {
      const auto h = measure::harmonics(Span(note(with(kQuiet, {{P::kWaveform, 7}, {P::kFmRatio, 4}, {P::kFmIndex, index}, {P::kFmSustainRate, 0}}), 0.6, 0.6).mono(),
                                              static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.35 * kRate)), kRate, midi_hz(60), 5);
      const double j0 = std::cyl_bessel_j(0.0, index), j1 = std::cyl_bessel_j(1.0, index);
      if (index == 0.0) expect(h[4] < 0.001 * h[0], "index 0 is a pure sine");
      else expect_near(measure::db(h[4] / h[0]), measure::db(std::abs(j1 / j0)), 0.5, "dB", fmt("index %.1f: J1/J0", index));
    }
  });
  prove(P::kWavetablePosition, "Table position scans the table: sine, triangle, saw, square", [] {
    const double f0 = midi_hz(48);
    auto at = [&](double position) {
      return harmonic_db(note(with(kQuiet, {{P::kWaveform, 46}, {P::kWavetablePosition, position}, {P::kWavetableWarp, 0.5}}), 0.6, 0.6, 48).mono(), f0, 3);
    };
    const auto sine = at(0.0), triangle = at(1.0 / 3.0), saw = at(2.0 / 3.0), square = at(1.0);
    expect(sine[1] < -40 && sine[2] < -40, fmt("0 is a sine (h2 %.1f, h3 %.1f dB)", sine[1], sine[2]));
    expect(triangle[1] < -35, "1/3 is a triangle: no 2nd harmonic");
    expect_near(triangle[2], -19.1, 1.0, "dB", "1/3 is a triangle: 3rd harmonic at 1/9");
    expect_near(saw[1], -6.0, 1.0, "dB", "2/3 is a saw: 2nd harmonic at 1/2");
    expect_near(saw[2], -9.5, 1.0, "dB", "2/3 is a saw: 3rd harmonic at 1/3");
    expect(square[1] < -35, "1 is a square: no 2nd harmonic");
    expect_near(square[2], -9.5, 1.0, "dB", "1 is a square: 3rd harmonic at 1/3");
    // On the additive voice it fades in the even harmonics.
    const auto odd = harmonic_db(note(with(kQuiet, {{P::kWaveform, 48}, {P::kWavetablePosition, 0}}), 0.6, 0.6, 48).mono(), f0, 2);
    const auto both = harmonic_db(note(with(kQuiet, {{P::kWaveform, 48}, {P::kWavetablePosition, 1}}), 0.6, 0.6, 48).mono(), f0, 2);
    expect(odd[1] < -40 && both[1] > -20, fmt("Additive: position 0 odd only (h2 %.1f dB), 1 adds evens (%.1f dB)", odd[1], both[1]));
  });
  prove(P::kWavetableWarp, "Table warp bends the cycle; centred it leaves the table untouched", [] {
    const double f0 = midi_hz(48);
    auto h2 = [&](double warp) {
      return harmonic_db(note(with(kQuiet, {{P::kWaveform, 46}, {P::kWavetablePosition, 0}, {P::kWavetableWarp, warp}}), 0.6, 0.6, 48).mono(), f0, 2)[1];
    };
    expect(h2(0.5) < -40, "Warp 0.5: the sine stays pure");
    expect(h2(0.0) > -20 && h2(1.0) > -20, "Warp 0 and 1: the bent sine gains a 2nd harmonic");
    expect(h2(0.25) < h2(0.0) && h2(0.75) < h2(1.0), "Warp bends more the further it is from centre");
  });
  prove(P::kAdditiveTilt, "Harmonic tilt sets how fast the additive harmonics fall off", [] {
    const double f0 = midi_hz(48);
    for (double tilt : {0.0, 0.45, 1.0}) {
      const auto h = harmonic_db(note(with(kQuiet, {{P::kWaveform, 48}, {P::kAdditiveTilt, tilt}, {P::kWavetablePosition, 1}}), 0.6, 0.6, 48).mono(), f0, 3);
      const double exponent = 0.35 + 2.65 * tilt;
      expect_near(h[1], -20 * exponent * std::log10(2.0), 0.7, "dB", fmt("tilt %.2f: 2nd harmonic", tilt));
      expect_near(h[2], -20 * exponent * std::log10(3.0), 0.7, "dB", fmt("tilt %.2f: 3rd harmonic", tilt));
    }
  });
  prove(P::kFmBrightness, "FM brightness raises the FM voices' brightness", [] {
    // Brightness is modulation depth: more of the harmonic series comes up to
    // within 30 dB of the loudest partial. The tine's brightness is its strike,
    // so it is measured early in the note.
    auto partials = [](const std::vector<float>& x, double from) {
      const auto h = harmonic_db(x, midi_hz(60), 16, from, 0.15);
      const double top = *std::max_element(h.begin(), h.end());
      return static_cast<int>(std::count_if(h.begin(), h.end(), [top](double v) { return v > top - 30; }));
    };
    // Brighter means the spectrum's centre rises or more partials come up (a
    // voice that already has every partial can only move its centre).
    for (int voice : {17, 27, 28, 29, 30, 49, 51, 55}) {
      std::vector<int> counts;
      std::vector<double> centres;
      const double from = voice == 55 ? 0.02 : 0.2;
      for (double b : {0.0, 0.5, 1.0}) {
        const auto x = note(with(kQuiet, {{P::kWaveform, static_cast<double>(voice)}, {P::kFmBrightness, b}}), 0.7, 0.7).mono();
        counts.push_back(partials(x, from));
        centres.push_back(measure::spectral_centroid(Span(x, static_cast<size_t>(from * kRate), 7200), kRate, 12));
      }
      const bool wider = counts[0] <= counts[1] && counts[1] <= counts[2] && counts[2] > counts[0];
      const bool higher = centres[1] > centres[0] * 1.02 && centres[2] > centres[1] * 1.02;
      expect(wider || higher, fmt("%s: brightness 0/0.5/1 is brighter (partials %d/%d/%d, centre %.0f/%.0f/%.0f Hz)", P::kWaveNames[voice],
                                  counts[0], counts[1], counts[2], centres[0], centres[1], centres[2]));
    }
  });
  prove(P::kLayerMode, "Layer adds its named partner voice", [] {
    const double f0 = midi_hz(60);
    const Patch sine = with(kQuiet, {{P::kWaveform, 46}, {P::kWavetablePosition, 0}, {P::kLayerMix, 0.5}});
    struct Case { int mode; double ratio; const char* name; };
    for (const Case c : {Case{1, 2.0, "Octave"}, Case{2, 1.5, "Fifth"}, Case{3, 0.5, "Sub octave"}}) {
      const auto x = note(with(sine, {{P::kLayerMode, static_cast<double>(c.mode)}}), 0.6, 0.6).mono();
      const Span body(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.35 * kRate));
      const double layer = measure::tone_amplitude(body, kRate, f0 * c.ratio), base = measure::tone_amplitude(body, kRate, f0);
      expect(layer > 0.3 * base, fmt("%s: partial at %.1fx the note (%.4f vs %.4f)", c.name, c.ratio, layer, base));
    }
    const auto off_layer = note(with(sine, {{P::kLayerMode, 0}}), 0.6, 0.6).mono();
    const Span quiet(off_layer, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.35 * kRate));
    expect(measure::tone_amplitude(quiet, kRate, 1.5 * f0) < 1e-4, "Off: no layer");
    // Triangle doubles at unison with a 1/9 third harmonic; Noise fills the spectrum between harmonics.
    const auto tri = harmonic_db(note(with(sine, {{P::kLayerMode, 4}}), 0.6, 0.6).mono(), f0, 3);
    expect(tri[2] > -40, fmt("Triangle: adds odd harmonics (h3 %.1f dB)", tri[2]));
    const auto noise = note(with(sine, {{P::kLayerMode, 5}}), 0.6, 0.6).mono();
    expect(measure::band_fraction(Span(noise, static_cast<size_t>(0.2 * kRate)), kRate, 2000, 20000) > 0.1, "Noise: broadband energy");
  });
  prove(P::kLayerMix, "Layer mix is the layer's share of the blend", [] {
    const double f0 = midi_hz(60);
    for (double mix : {0.25, 0.5, 0.75}) {
      const auto x = note(with(kQuiet, {{P::kWaveform, 46}, {P::kWavetablePosition, 0}, {P::kLayerMode, 2}, {P::kLayerMix, mix}}), 0.6, 0.6).mono();
      const Span body(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.35 * kRate));
      // Sine voice and sine fifth: (1 - mix) * voice + mix * layer.
      expect_near(measure::db(measure::tone_amplitude(body, kRate, 1.5 * f0) / measure::tone_amplitude(body, kRate, f0)),
                  measure::db(mix / (1 - mix)), 0.3, "dB", fmt("Layer mix %.2f: fifth vs voice", mix));
    }
  });
  prove(P::kChipCutoff, "Chip cutoff is the chip filter's cutoff in Hz (12 dB/octave low-pass)", [] {
    // Measured against the same voice with the filter wide open, so only the filter shows.
    for (int voice : {39, 52}) {
      const Patch base = with(kQuiet, {{P::kWaveform, static_cast<double>(voice)}, {P::kChipResonance, 0}, {P::kExpansionShape, 0}, {P::kReleaseMs, 0}});
      const double f0 = midi_hz(36);
      const auto open = measure::harmonics(Span(note(with(base, {{P::kChipCutoff, 16000}}), 0.6, 0.6, 36).mono(), 9600, 14400), kRate, f0, 100);
      for (double cutoff : {500.0, 1500.0}) {
        const auto shut = measure::harmonics(Span(note(with(base, {{P::kChipCutoff, cutoff}}), 0.6, 0.6, 36).mono(), 9600, 14400), kRate, f0, 100);
        auto response = [&](double hz) { const size_t k = static_cast<size_t>(std::lround(hz / f0)) - 1; return measure::db(shut[k] / open[k]); };
        expect(response(cutoff / 4) > -2.0, fmt("%s cutoff %.0f Hz: two octaves below passes (%.1f dB)", P::kWaveNames[voice], cutoff, response(cutoff / 4)));
        expect_near(response(cutoff), -3.8, 2.5, "dB", fmt("%s cutoff %.0f Hz: response at the cutoff", P::kWaveNames[voice], cutoff));
        expect_near(response(cutoff * 2) - response(cutoff), -9.0, 4.0, "dB", fmt("%s cutoff %.0f Hz: next octave falls", P::kWaveNames[voice], cutoff));
      }
    }
  });
  prove(P::kChipResonance, "Chip resonance boosts the filter at its cutoff", [] {
    const Patch base = with(kQuiet, {{P::kWaveform, 52}, {P::kChipCutoff, 1000}, {P::kExpansionShape, 0}});
    const double f0 = midi_hz(36);
    double previous = -1e9;
    for (double r : {0.0, 0.5, 0.9}) {
      const auto h = measure::harmonics(Span(note(with(base, {{P::kChipResonance, r}}), 0.6, 0.6, 36).mono(), 9600, 14400), kRate, f0, 20);
      const double at_cutoff = measure::db(h[static_cast<size_t>(std::lround(1000 / f0)) - 1] / h[0]);
      expect(at_cutoff > previous + 1.0, fmt("Resonance %.1f: level at the cutoff rises (%.1f dB re fundamental)", r, at_cutoff));
      previous = at_cutoff;
    }
  });
  prove(P::kCustomWave, "Custom wave replaces the source with the drawn 32-step table", [] {
    for (int source : {0, 10, 46}) {
      // A drawn half-cycle square: low for 16 steps, high for 16.
      Patch patch = {{P::kWaveform, static_cast<double>(source)}, {P::kCustomWave, 1}, {P::kAttackMs, 0}, {P::kHardwareEnvelope, 0}};
      for (uint32_t k = 0; k < 32; ++k) patch.push_back({P::kWaveSample1 + k, k < 16 ? 0.0 : 15.0});
      const auto x = note(patch, 0.6, 0.6, 36).mono();
      expect_near(measure::duty(Span(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.3 * kRate))), 0.5, 0.02, "",
                  fmt("%s with Custom wave plays the drawn square", P::kWaveNames[source]));
      Patch off_patch = patch;
      off_patch[1].value = 0;
      const auto y = note(off_patch, 0.6, 0.6, 36).mono();
      expect(measure::correlation(Span(x, 9600, 9600), Span(y, 9600, 9600)) < 0.9, fmt("%s: Off restores the source", P::kWaveNames[source]));
    }
  });
  for (uint32_t k = 0; k < 32; ++k) {
    prove(P::kWaveSample1 + k, "Wave sample is the level of that 1/32 of the cycle", [k] {
      for (double level : {15.0, 0.0}) {
        Patch patch = {{P::kWaveform, 58}, {P::kAttackMs, 0}};
        for (uint32_t j = 0; j < 32; ++j) patch.push_back({P::kWaveSample1 + j, j == k ? level : (level > 7 ? 0.0 : 15.0)});
        // Key 36 (65.4 Hz): 734 samples per cycle, about 23 per step. The cycle starts at note-on.
        const auto x = note(patch, 0.5, 0.5, 36).mono();
        const double period = kRate / midi_hz(36);
        std::vector<double> step(32, 0.0);
        for (int cycle = 10; cycle < 20; ++cycle)
          for (int j = 0; j < 32; ++j) {
            const size_t from = static_cast<size_t>((cycle + (j + 0.25) / 32.0) * period), to = static_cast<size_t>((cycle + (j + 0.75) / 32.0) * period);
            step[static_cast<size_t>(j)] += measure::mean(Span(x, from, to - from));
          }
        const size_t extreme = level > 7 ? static_cast<size_t>(std::max_element(step.begin(), step.end()) - step.begin())
                                         : static_cast<size_t>(std::min_element(step.begin(), step.end()) - step.begin());
        expect(extreme == k, fmt("Wave sample %u at %.0f is heard at step %zu of the cycle", k + 1, level, extreme + 1));
      }
    });
  }
}

}  // namespace

namespace {

// Level (dB) of a note's envelope every millisecond, relative to its peak.
std::vector<double> level_db(const std::vector<float>& x, bool ac = false) {
  auto env = envelope_ms(x, ac);
  const double top = *std::max_element(env.begin(), env.end());
  std::vector<double> out;
  for (double e : env) out.push_back(measure::db(e / top));
  return out;
}
double ms_to_fall_to(const std::vector<double>& db, double threshold, size_t from = 0) {
  for (size_t i = from; i < db.size(); ++i) if (db[i] <= threshold) return static_cast<double>(i - from);
  return 1e9;
}

// YM2612 with the four operators as carriers at 1-4x (algorithm 7), unless overridden.
const Patch kGenesis = with(kQuiet, {{P::kWaveform, 17}, {P::kGenesisAlgorithm, 7}, {P::kGenesisFeedback, 0},
                                     {P::kFmAttack, 31}, {P::kFmDecay, 0}, {P::kFmSustainRate, 0},
                                     {P::kFmSustainLevel, 0}, {P::kFmRelease, 15}, {P::kReleaseMs, 0}});
// For level proofs: one carrier (algorithm 0), so four full carriers cannot clip the
// chip's output accumulator and squash what is being measured.
const Patch kGenesisLevel = with(kGenesis, {{P::kGenesisAlgorithm, 0}});

void register_fm() {
  prove(P::kGenesisAlgorithm, "FM algorithm selects the operator routing (8 on 4-op chips, 32 on the six-op voice)", [] {
    for (int voice : {17, 30}) {
      std::vector<std::vector<float>> renders;
      for (int algorithm = 0; algorithm < 8; ++algorithm) {
        renders.push_back(note(with(kGenesis, {{P::kWaveform, static_cast<double>(voice)}, {P::kGenesisAlgorithm, static_cast<double>(algorithm)}}), 0.6, 0.6).mono());
        expect(rms_db(renders.back(), 0.2, 0.3) > -60, fmt("%s algorithm %d sounds", P::kWaveNames[voice], algorithm));
      }
      for (size_t a = 0; a < renders.size(); ++a)
        for (size_t b = a + 1; b < renders.size(); ++b)
          expect(timbre_similarity(renders[a], renders[b]) < 0.999, fmt("%s algorithms %zu and %zu differ", P::kWaveNames[voice], a, b));
    }
    std::vector<std::vector<float>> six;
    for (int algorithm = 0; algorithm < 32; ++algorithm)
      six.push_back(note(with(kQuiet, {{P::kWaveform, 49}, {P::kGenesisAlgorithm, static_cast<double>(algorithm)}, {P::kFmIndex, 2}}), 0.5, 0.5).mono());
    for (size_t a = 0; a < six.size(); ++a)
      for (size_t b = a + 1; b < six.size(); ++b)
        expect(measure::correlation(Span(six[a], 9600, 4800), Span(six[b], 9600, 4800)) < 0.999,
               fmt("Six-operator FM algorithms %zu and %zu differ", a, b));
  });
  prove(P::kGenesisFeedback, "FM feedback brightens operator 1 as it rises", [] {
    double previous = 0;
    // Operator 1 feeds back on itself: the more feedback, the further its tone
    // moves from the plain sine it is at 0.
    const auto plain = note(with(kGenesis, {{P::kGenesisFeedback, 0}}), 0.6, 0.6).mono();
    previous = 1.0;
    for (double fb : {2.0, 4.0, 6.0}) {
      const double similarity = timbre_similarity(note(with(kGenesis, {{P::kGenesisFeedback, fb}}), 0.6, 0.6).mono(), plain);
      expect(similarity < previous - 0.005, fmt("feedback %.0f moves further from the sine (similarity %.4f)", fb, similarity));
      previous = similarity;
    }
  });
  prove(P::kFmAttack, "FM attack is the operators' attack rate: higher is faster, 31 instant", [] {
    double previous = 1e9;
    for (double ar : {6.0, 10.0, 14.0, 31.0}) {
      const auto db = level_db(note(with(kGenesisLevel, {{P::kFmAttack, ar}}), 1.5, 1.5).mono(), true);
      const double rise = static_cast<double>(measure::first_at_or_above(db, -1.0));
      expect(rise < previous, fmt("attack rate %.0f is faster (%.0f ms to full)", ar, rise));
      previous = rise;
    }
    expect(previous <= 4, fmt("attack rate 31 is instant (%.0f ms)", previous));
  });
  prove(P::kFmDecay, "FM decay is the rate the level falls to the sustain level", [] {
    double previous = 1e9;
    for (double dr : {8.0, 12.0, 16.0}) {
      const double t = ms_to_fall_to(level_db(note(with(kGenesisLevel, {{P::kFmDecay, dr}, {P::kFmSustainLevel, 8}}), 3.0, 3.0).mono(), true), -12.0);
      expect(t < previous, fmt("decay rate %.0f is faster (%.0f ms to -12 dB)", dr, t));
      previous = t;
    }
  });
  prove(P::kFmSustainLevel, "FM sustain level is where the decay stops, 3 dB per step", [] {
    // Absolute held levels (a fast decay reaches the sustain level at once): each
    // step down is 3 dB.
    std::vector<double> held;
    for (double sl : {0.0, 2.0, 4.0, 8.0})
      held.push_back(rms_db(note(with(kGenesisLevel, {{P::kFmDecay, 31}, {P::kFmSustainLevel, sl}}), 0.8, 0.8).mono(), 0.4, 0.3));
    expect_near(held[1] - held[0], -6.0, 1.0, "dB", "sustain level 0 -> 2");
    expect_near(held[2] - held[1], -6.0, 1.0, "dB", "sustain level 2 -> 4");
    expect_near(held[3] - held[2], -12.0, 2.0, "dB", "sustain level 4 -> 8");
  });
  prove(P::kFmSustainRate, "FM sustain rate is the rate the level keeps falling while held", [] {
    // The FM-family synth voices decay at 4.4 dB/s at rate 5, doubling every two rates; 0 holds.
    for (int voice : {7, 49, 51, 55}) {
      for (double sr : {0.0, 5.0, 9.0}) {
        const auto x = note(with(kQuiet, {{P::kWaveform, static_cast<double>(voice)}, {P::kFmSustainRate, sr}, {P::kAttackMs, 0}}), 1.6, 1.6).mono();
        const double slope = (rms_db(x, 1.3, 0.2) - rms_db(x, 0.3, 0.2)) / 1.0;
        expect_near(-slope, sr == 0 ? 0.0 : 4.4 * std::exp2((sr - 5.0) / 2.0), 0.6 + 0.05 * std::abs(slope), "dB/s",
                    fmt("%s at sustain rate %.0f: held decay", P::kWaveNames[voice], sr));
      }
    }
    double previous = -1;
    for (double sr : {0.0, 8.0, 14.0}) {
      const auto db = level_db(note(with(kGenesisLevel, {{P::kFmSustainRate, sr}}), 2.0, 2.0).mono(), true);
      const double drop = db[200] - db[1800];
      if (sr == 0.0) expect(std::abs(drop) < 0.5, fmt("sustain rate 0 holds (%.1f dB over 1.6 s)", drop));
      else expect(drop > previous + 1.0, fmt("sustain rate %.0f falls faster (%.1f dB over 1.6 s)", sr, drop));
      previous = drop;
    }
  });
  prove(P::kFmRelease, "FM release is the operators' release rate: higher fades faster", [] {
    double previous = 1e9;
    // The chip's own release ends a hardware FM note: the voice Release knob does not cut it short.
    for (double rr : {6.0, 9.0, 12.0}) {
      const double t = ms_to_fall_to(level_db(note(with(kGenesisLevel, {{P::kFmRelease, rr}}), 0.3, 3.0).mono(), true), -40.0, 300);
      expect(t < previous, fmt("release rate %.0f fades faster (%.0f ms to -40 dB)", rr, t));
      previous = t;
    }
  });
  prove(P::kFmDetune, "FM detune offsets the operators' pitch: 1-3 up, 5-7 down", [] {
    auto cents_at = [](double dt) {
      const auto x = note(with(kGenesis, {{P::kFmDetune, dt}}), 0.8, 0.8, 96).mono();
      return measure::cents(pitch_at(x, 0.2, 0.5), midi_hz(96));
    };
    const double none = cents_at(0), up = cents_at(3), down = cents_at(7);
    expect(up > none + 0.5, fmt("detune 3 is sharp (%.2f vs %.2f cents)", up, none));
    expect(down < none - 0.5, fmt("detune 7 is flat (%.2f vs %.2f cents)", down, none));
  });
  prove(P::kFmKeyScale, "FM key scale speeds the envelope up for higher notes", [] {
    auto decay = [](double ks, int key) {
      return ms_to_fall_to(level_db(note(with(kGenesisLevel, {{P::kFmKeyScale, ks}, {P::kFmDecay, 10}, {P::kFmSustainLevel, 15}}), 3.0, 3.0, key).mono(), true), -20.0);
    };
    const double flat = decay(0, 96), scaled = decay(3, 96);
    expect(scaled < 0.7 * flat, fmt("key scale 3 decays a high note faster (%.0f vs %.0f ms)", scaled, flat));
  });
  prove(P::kFmLfoRate, "FM LFO rate selects the chip's LFO frequency", [] {
    // YM2612 LFO frequencies at the 7.67 MHz Genesis clock.
    constexpr double hz[] = {3.98, 5.56, 6.02, 6.37, 6.88, 9.63};
    for (int rate = 0; rate < 6; ++rate) {
      const auto x = note(with(kGenesis, {{P::kFmLfoRate, static_cast<double>(rate)}, {P::kFmPmDepth, 127}}), 2.0, 2.0).mono();
      expect_near(measure::modulation_hz(measure::pitch_track(x, kRate, 0.002, 0.012), 0.002, 1.0, 30.0), hz[rate], 0.06 * hz[rate], "Hz",
                  fmt("LFO rate %d", rate));
    }
  });
  prove(P::kFmAmDepth, "FM AM depth is tremolo from the chip LFO", [] {
    double previous = -1;
    for (double depth : {0.0, 64.0, 127.0}) {
      const auto db = level_db(note(with(kGenesisLevel, {{P::kFmAmDepth, depth}, {P::kFmLfoRate, 3}}), 2.0, 2.0).mono(), true);
      const double swing = *std::max_element(db.begin() + 300, db.begin() + 1900) - *std::min_element(db.begin() + 300, db.begin() + 1900);
      if (depth == 0.0) expect(swing < 0.5, fmt("AM depth 0: steady level (%.2f dB)", swing));
      else expect(swing > previous + 1.0, fmt("AM depth %.0f: deeper tremolo (%.1f dB)", depth, swing));
      previous = swing;
    }
  });
  prove(P::kFmPmDepth, "FM PM depth is vibrato from the chip LFO", [] {
    double previous = -1;
    for (double depth : {0.0, 64.0, 127.0}) {
      const auto track = semitone_track(note(with(kGenesis, {{P::kFmPmDepth, depth}, {P::kFmLfoRate, 3}}), 2.0, 2.0).mono(), 0.002, 0.012);
      double lo = 1e9, hi = -1e9;
      for (size_t i = 150; i + 10 < track.size(); ++i) if (!std::isnan(track[i])) { lo = std::min(lo, track[i]); hi = std::max(hi, track[i]); }
      const double swing = (hi - lo) * 100;
      if (depth == 0.0) expect(swing < 2, fmt("PM depth 0: steady pitch (%.1f cents)", swing));
      else expect(swing > previous + 2, fmt("PM depth %.0f: deeper vibrato (%.1f cents)", depth, swing));
      previous = swing;
    }
  });
}

// ----- DPCM: real 1-bit sample banks ------------------------------------------

// NTSC DMC periods in CPU cycles.
constexpr double kDmcPeriods[] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 85, 72, 54};

// Writes raw DMC banks and points the plug-in at them (it reads the list at init).
struct Banks {
  std::filesystem::path dir;
  explicit Banks(const std::vector<std::vector<uint8_t>>& slots) {
    static int serial = 0;
    dir = std::filesystem::temp_directory_path() /
          ("yanes-advertised-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" + std::to_string(++serial));
    std::filesystem::create_directories(dir);
    std::string list;
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto path = dir / ("slot" + std::to_string(i) + ".dmc");
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(slots[i].data()), static_cast<std::streamsize>(slots[i].size()));
#ifdef _WIN32
      if (!list.empty()) list += ';';
#else
      if (!list.empty()) list += ':';
#endif
      list += path.string();
    }
    set_bank_list(list.c_str());
  }
  ~Banks() { set_bank_list(""); std::filesystem::remove_all(dir); }
  static void set_bank_list(const char* list) {
#ifdef _WIN32
    _putenv_s("YANES_DPCM_BANK", list);
#else
    if (*list) ::setenv("YANES_DPCM_BANK", list, 1);
    else ::unsetenv("YANES_DPCM_BANK");
#endif
  }
};

// A triangle-ish tone: `half` bits up, `half` down, repeated to `bytes`.
std::vector<uint8_t> dmc_tone(int half_bytes, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (size_t i = 0; i < bytes; ++i) out[i] = (i / static_cast<size_t>(half_bytes)) % 2 ? 0x00 : 0xff;
  return out;
}

double sounding_seconds(const std::vector<float>& x) {
  const auto db = level_db(x);
  return static_cast<double>(measure::last_at_or_above(db, -30)) / 1000.0;
}

const Patch kDpcm = {{P::kWaveform, 9}, {P::kDpcmBaseKey, 60}, {P::kAttackMs, 0}, {P::kReleaseMs, 0}};

void register_dpcm() {
  prove(P::kDpcmRate, "DPCM rate plays the sample at the chip's DMC rate", [] {
    Banks banks({dmc_tone(2, 128)});  // 1024 bits
    for (int rate : {6, 10, 15}) {
      const auto x = note(with(kDpcm, {{P::kDpcmRate, static_cast<double>(rate)}}), 2.0, 2.0).mono();
      expect_near(sounding_seconds(x), 1024.0 * kDmcPeriods[rate] / 1789773.0, 0.004, "s", fmt("DPCM rate %d: sample length", rate));
    }
  });
  prove(P::kDpcmBaseKey, "DPCM base key is the key that plays slot 1", [] {
    Banks banks({dmc_tone(1, 256), dmc_tone(4, 256)});  // slot 1 pitched two octaves below slot 0
    for (double base : {60.0, 59.0}) {
      const auto x = note(with(kDpcm, {{P::kDpcmBaseKey, base}, {P::kDpcmRate, 15}}), 0.5, 0.5).mono();
      // Slot 0 cycles every 16 bits, slot 1 every 64: 33144/16 and 33144/64 Hz.
      const double expected = 1789773.0 / 54.0 / (base == 60.0 ? 16.0 : 64.0);
      expect_near(measure::cents(pitch_at(x, 0.02, 0.1), expected), 0, 20, "cents",
                  fmt("base key %.0f: key 60 plays slot %d", base, base == 60.0 ? 1 : 2));
    }
  });
  prove(P::kDpcmLoopMask, "DPCM loop mask bit N loops slot N+1", [] {
    Banks banks({dmc_tone(2, 64)});  // 512 bits, 15 ms at rate 15
    for (double mask : {0.0, 1.0}) {
      const auto x = note(with(kDpcm, {{P::kDpcmRate, 15}, {P::kDpcmLoopMask, mask}}), 0.5, 0.6).mono();
      if (mask > 0) expect(rms_db(x, 0.3, 0.1) > -40, "bit set: the sample loops while the key is held");
      else expect(rms_db(x, 0.3, 0.1) < -80, "bit clear: the sample plays once");
    }
  });
  prove(P::kDpcmInitialLevel, "DPCM initial level is the DAC's starting level", [] {
    // An all-zero sample walks the DAC down by 2 per bit from where it starts.
    Banks banks({std::vector<uint8_t>(64, 0x00)});
    for (double level : {40.0, 100.0}) {
      const auto x = note(with(kDpcm, {{P::kDpcmRate, 15}, {P::kDpcmInitialLevel, level}}), 0.2, 0.2).mono();
      // The walk takes level / 2 bits to reach the floor, then holds there.
      const double bits = level / 2.0, seconds = bits * 54.0 / 1789773.0;
      const double start = x[1], floor = x[static_cast<size_t>((seconds + 0.003) * kRate)];
      expect_near(x[static_cast<size_t>(seconds / 2 * kRate)] - floor, (start - floor) / 2, std::abs(start - floor) * 0.1, "",
                  fmt("initial level %.0f: halfway down at %.2f ms", level, seconds * 500));
    }
  });
  prove(P::kDpcmTrimStart, "DPCM trim start skips that fraction of the sample", [] {
    Banks banks({dmc_tone(2, 128)});
    for (double start : {0.0, 0.25, 0.5}) {
      const auto x = note(with(kDpcm, {{P::kDpcmRate, 10}, {P::kDpcmTrimStart, start}}), 2.0, 2.0).mono();
      expect_near(sounding_seconds(x), (1.0 - start) * 1024.0 * kDmcPeriods[10] / 1789773.0, 0.004, "s", fmt("trim start %.0f%%", start * 100));
    }
  });
  prove(P::kDpcmTrimEnd, "DPCM trim end stops at that fraction of the sample", [] {
    Banks banks({dmc_tone(2, 128)});
    for (double end : {1.0, 0.75, 0.5}) {
      const auto x = note(with(kDpcm, {{P::kDpcmRate, 10}, {P::kDpcmTrimEnd, end}}), 2.0, 2.0).mono();
      expect_near(sounding_seconds(x), end * 1024.0 * kDmcPeriods[10] / 1789773.0, 0.004, "s", fmt("trim end %.0f%%", end * 100));
    }
  });
}

// ----- effects and output -------------------------------------------------------

// A 20 ms blip, so echoes and delays stand apart from the dry sound.
std::vector<float> blip(const Patch& patch, double seconds = 1.5) {
  return render(with(kClean, patch), {on(0.05), off(0.07)}, seconds).mono();
}
double peak_in(const std::vector<float>& x, double from, double to) {
  return measure::peak(Span(x, static_cast<size_t>(from * kRate), static_cast<size_t>((to - from) * kRate)));
}

// The console/TV chain with everything but the setting under test neutral.
const Patch kRetro = with(kClean, {{P::kGainDb, -18}, {P::kRetroAmount, 1}, {P::kBitDepth, 16}, {P::kOutputRate, 48000},
                                   {P::kRfNoise, 0}, {P::kHum, 0}, {P::kSpeaker, 0}});

double residual_db(const std::vector<float>& a, const std::vector<float>& b, double from = 0.2, double length = 0.4) {
  std::vector<float> d(a.size());
  for (size_t i = 0; i < d.size(); ++i) d[i] = a[i] - b[i];
  return rms_db(d, from, length);
}

void register_effects() {
  prove(P::kDrive, "Drive saturates the output: more drive, more distortion; 0 is clean", [] {
    double previous = -1e9;
    for (double d : {0.0, 0.3, 0.7, 1.0}) {
      const double h3 = harmonic_db(note(with(kQuiet, {{P::kWaveform, 46}, {P::kWavetablePosition, 0}, {P::kGainDb, -18}, {P::kDrive, d}}), 0.6, 0.6).mono(),
                                    midi_hz(60), 3)[2];
      if (d == 0.0) expect(h3 < -45, fmt("drive 0 is clean (h3 %.1f dB)", h3));
      else expect(h3 > previous + 2.0, fmt("drive %.1f distorts more (h3 %.1f dB)", d, h3));
      previous = h3;
    }
  });
  prove(P::kEchoMix, "Echo mix is the echo's share of the output", [] {
    for (double mix : {0.25, 0.5}) {
      const auto x = blip({{P::kEchoMix, mix}, {P::kEchoTime, 300}, {P::kEchoFeedback, 0}});
      expect_near(measure::db(peak_in(x, 0.35, 0.38) / peak_in(x, 0.05, 0.08)), measure::db(mix / (1 - mix)), 0.5, "dB",
                  fmt("mix %.2f: echo vs dry", mix));
    }
    expect(peak_in(blip({{P::kEchoMix, 0}, {P::kEchoTime, 300}}), 0.3, 0.45) < 1e-6, "mix 0: no echo");
  });
  prove(P::kEchoTime, "Echo time is the delay of the echo in ms", [] {
    for (double t : {50.0, 180.0, 700.0}) {
      const auto env = envelope_ms(blip({{P::kEchoMix, 0.5}, {P::kEchoTime, t}, {P::kEchoFeedback, 0}}));
      expect_near(time_to_reach(env, 0.5, env[60], 80) - time_to_reach(env, 0.5, env[60], 0), t, 1.5, "ms", fmt("echo time %.0f ms", t));
    }
  });
  prove(P::kEchoFeedback, "Echo feedback is the level of each repeat relative to the last", [] {
    for (double fb : {0.3, 0.6, 0.9}) {
      const auto x = blip({{P::kEchoMix, 0.5}, {P::kEchoTime, 200}, {P::kEchoFeedback, fb}}, 1.0);
      expect_near(measure::db(peak_in(x, 0.45, 0.48) / peak_in(x, 0.25, 0.28)), measure::db(fb), 0.5, "dB", fmt("feedback %.1f: 2nd vs 1st repeat", fb));
    }
  });
  prove(P::kChorusMix, "Chorus mix blends in the modulated, delayed copy", [] {
    const auto dry = render(with(kClean, {{P::kChorusMix, 0}}), {on(0), off(1.0)}, 1.0);
    const auto wet = render(with(kClean, {{P::kChorusMix, 1}, {P::kChorusDepth, 4}}), {on(0), off(1.0)}, 1.0);
    expect(measure::correlation(Span(dry.left, 9600, 24000), Span(dry.right, 9600, 24000)) > 0.9999, "mix 0: left and right identical");
    expect(measure::correlation(Span(wet.left, 9600, 24000), Span(wet.right, 9600, 24000)) < 0.95, "mix 1: the two chorus taps differ");
    // Fully wet is only the delayed copy: nothing comes out for the 14 ms base delay.
    const auto env = envelope_ms(wet.mono());
    expect(time_to_reach(env, 0.3, env[300]) >= 13.0, fmt("mix 1: output starts after the chorus delay (%.0f ms)", time_to_reach(env, 0.3, env[300])));
  });
  prove(P::kChorusRate, "Chorus rate is the chorus modulation frequency in Hz", [] {
    for (double rate : {1.0, 3.0}) {
      const auto x = render(with(kClean, {{P::kChorusMix, 1}, {P::kChorusDepth, 8}, {P::kChorusRate, rate}}), {on(0), off(3.0)}, 3.0).left;
      expect_near(measure::modulation_hz(measure::pitch_track(x, kRate, 0.004, 0.02), 0.004, 0.3, 20.0), rate, 0.06 * rate, "Hz", fmt("chorus rate %.0f Hz", rate));
    }
  });
  prove(P::kChorusDepth, "Chorus depth is the delay swing in ms", [] {
    // A delay swinging by D seconds at R Hz bends pitch by up to pi * R * D.
    for (double depth : {4.0, 10.0}) {
      const auto track = semitone_track(render(with(kClean, {{P::kChorusMix, 1}, {P::kChorusDepth, depth}, {P::kChorusRate, 1}}), {on(0), off(3.0)}, 3.0).left,
                                        0.004, 0.02);
      double hi = 0;
      for (size_t i = 100; i + 10 < track.size(); ++i) if (!std::isnan(track[i])) hi = std::max(hi, std::abs(track[i]));
      expect_near(hi * 100, measure::cents(1.0 + measure::kPi * 1.0 * depth / 1000, 1.0), 0.2 * measure::cents(1.0 + measure::kPi * depth / 1000, 1.0), "cents",
                  fmt("chorus depth %.0f ms: peak pitch bend", depth));
    }
  });
  prove(P::kStereoWidth, "Stereo width spreads the sound across left and right", [] {
    const auto narrow = render(with(kClean, {{P::kStereoWidth, 0}}), {on(0), off(0.6)}, 0.6);
    const auto wide = render(with(kClean, {{P::kStereoWidth, 1}}), {on(0), off(0.6)}, 0.6);
    expect(measure::correlation(Span(narrow.left, 9600, 9600), Span(narrow.right, 9600, 9600)) > 0.9999, "width 0: mono");
    const double lr = measure::correlation(Span(wide.left, 9600, 9600), Span(wide.right, 9600, 9600));
    expect(lr < 0.95, fmt("width 1: left and right decorrelate (correlation %.4f)", lr));
  });
  prove(P::kRetroAmount, "Retro amount blends in the console/TV chain", [] {
    const Patch rough = with(kRetro, {{P::kBitDepth, 5}});
    const auto clean = note(with(rough, {{P::kRetroAmount, 0}}), 0.6, 0.6).mono();
    const auto half = note(with(rough, {{P::kRetroAmount, 0.5}}), 0.6, 0.6).mono();
    const auto full = note(with(rough, {{P::kRetroAmount, 1}}), 0.6, 0.6).mono();
    expect(residual_db(clean, note(kClean, 0.6, 0.6).mono()) < -100 || rms_db(clean, 0.2, 0.4) > -200, "amount 0 is bypass");
    expect_near(residual_db(half, clean) - residual_db(full, clean), -6.02, 0.5, "dB", "amount 0.5 is half the processed signal");
  });
  prove(P::kBitDepth, "Bit depth quantizes the output: 6 dB of noise per bit removed", [] {
    const auto reference = note(with(kRetro, {{P::kBitDepth, 16}}), 0.6, 0.6).mono();
    const double e6 = residual_db(note(with(kRetro, {{P::kBitDepth, 6}}), 0.6, 0.6).mono(), reference);
    const double e10 = residual_db(note(with(kRetro, {{P::kBitDepth, 10}}), 0.6, 0.6).mono(), reference);
    expect_near(e6 - e10, 24.08, 3.0, "dB", "6 bits vs 10 bits: quantization error");
  });
  prove(P::kOutputRate, "Output rate sample-and-holds at that rate, imaging around it", [] {
    const double f0 = midi_hz(69);
    for (double rate : {8000.0, 16000.0}) {
      const auto x = note(with(kRetro, {{P::kWavetablePosition, 0}, {P::kOutputRate, rate}}), 0.6, 0.6, 69).mono();
      const Span body(x, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.3 * kRate));
      const double image = measure::tone_amplitude(body, kRate, rate - f0), tone = measure::tone_amplitude(body, kRate, f0);
      // A zero-order hold images f0 at rate - f0 at f0 / (rate - f0) of its level; the
      // chain's 14 kHz one-pole low-pass then shapes both.
      auto lowpass = [](double hz) {
        const double a = 1.0 - std::exp(-2.0 * measure::kPi * 14000.0 / kRate);
        const std::complex<double> z = std::polar(1.0, -2.0 * measure::kPi * hz / kRate);
        return std::abs(a / (1.0 - (1.0 - a) * z));
      };
      const double expected = measure::db(f0 / (rate - f0) * lowpass(rate - f0) / lowpass(f0));
      expect_near(measure::db(image / tone), expected, 2.0, "dB", fmt("rate %.0f: image at %.0f Hz", rate, rate - f0));
    }
    const auto full = note(with(kRetro, {{P::kWavetablePosition, 0}, {P::kOutputRate, 48000}}), 0.6, 0.6, 69).mono();
    const Span body(full, static_cast<size_t>(0.2 * kRate), static_cast<size_t>(0.3 * kRate));
    expect(measure::tone_amplitude(body, kRate, 8000 - f0) < 1e-3 * measure::tone_amplitude(body, kRate, f0), "48 kHz: no image");
  });
  prove(P::kRfNoise, "RF noise adds broadband noise in proportion", [] {
    const auto reference = note(kRetro, 0.6, 0.6).mono();
    const double half = residual_db(note(with(kRetro, {{P::kRfNoise, 0.5}}), 0.6, 0.6).mono(), reference);
    const double full = residual_db(note(with(kRetro, {{P::kRfNoise, 1}}), 0.6, 0.6).mono(), reference);
    expect_near(full - half, 6.02, 1.0, "dB", "RF noise 1 vs 0.5");
  });
  prove(P::kHum, "Mains hum adds a mains-frequency hum in proportion", [] {
    std::vector<double> level;
    for (double hum : {0.5, 1.0}) {
      const auto x = note(with(kRetro, {{P::kHum, hum}, {P::kGainDb, -36}}), 1.0, 1.0).mono();
      level.push_back(measure::tone_amplitude(Span(x, 12000, 24000), kRate, 60));
    }
    expect_near(measure::db(level[1] / level[0]), 6.02, 0.5, "dB", "hum 1 vs 0.5 at 60 Hz");
  });
  prove(P::kSpeaker, "TV speaker narrows the band: highs fall as it rises", [] {
    double previous = 1;
    for (double speaker : {0.0, 0.5, 1.0}) {
      const auto x = note(with(kRetro, {{P::kWavetablePosition, 1}, {P::kSpeaker, speaker}}), 0.6, 0.6).mono();
      const double high = measure::band_fraction(Span(x, 9600, 14400), kRate, 4000, 20000);
      expect(high < previous * 0.8, fmt("speaker %.1f: less energy above 4 kHz (%.4f)", speaker, high));
      previous = high;
    }
  });
  auto channel_levels = [](const Patch& patch) {
    const auto x = render(with(kClean, patch), {on(0, 60, 0), on(0, 67, 1), on(0, 72, 5), off(0.6, 60, 0), off(0.6, 67, 1), off(0.6, 72, 5)}, 0.6).mono();
    const Span body(x, 9600, 14400);
    return std::array<double, 3>{measure::tone_amplitude(body, kRate, midi_hz(60)), measure::tone_amplitude(body, kRate, midi_hz(67)),
                                 measure::tone_amplitude(body, kRate, midi_hz(72))};
  };
  prove(P::kStackMuteMask, "Channel mute mask bit N silences MIDI channel N+1", [channel_levels] {
    const auto all = channel_levels({{P::kStackMuteMask, 0}});
    const auto muted = channel_levels({{P::kStackMuteMask, (1 << 1) | (1 << 5)}});
    expect(all[1] > 0.01 && all[2] > 0.01, "mask 0: every channel sounds");
    expect(muted[0] > 0.9 * all[0], "mask 0x22: channel 1 still sounds");
    expect(muted[1] < 1e-4 && muted[2] < 1e-4, "mask 0x22: channels 2 and 6 are silent");
  });
  prove(P::kStackSoloMask, "Channel solo mask plays only the channels whose bits are set", [channel_levels] {
    const auto all = channel_levels({{P::kStackSoloMask, 0}});
    const auto solo = channel_levels({{P::kStackSoloMask, 1 << 5}});
    expect(all[0] > 0.01 && all[1] > 0.01 && all[2] > 0.01, "mask 0: no solo, every channel sounds");
    expect(solo[0] < 1e-4 && solo[1] < 1e-4 && solo[2] > 0.9 * all[2], "mask 0x20: only channel 6 sounds");
  });
}

}  // namespace

namespace {

// ----- sources ------------------------------------------------------------------

enum class Kind { kTonal, kNoise, kPercussive };

Kind kind_of(int waveform) {
  switch (waveform) {
    case 2: case 12: case 14: case 16: case 23: case 25: case 37: return Kind::kNoise;
    case 9: case 57: case 100: return Kind::kPercussive;
    default: return Kind::kTonal;
  }
}

// What a source (or a stack channel playing it) must sound like at C-4.
void expect_kind(const std::vector<float>& x, Kind kind, const std::string& what) {
  const double level = rms_db(x, 0.05, 0.3);
  expect(level > -50, fmt("%s is audible (%.1f dBFS)", what.c_str(), level));
  if (kind == Kind::kTonal && what == P::kWaveNames[56]) {
    // The ladder mono synth carries a sub-oscillator an octave down: the played
    // oscillator must still be the loudest partial.
    const Span body(x, 9600, 9600);
    expect(measure::tone_amplitude(body, kRate, midi_hz(60)) > measure::tone_amplitude(body, kRate, midi_hz(48)),
           what + " plays C-4 over its sub-oscillator");
  } else if (kind == Kind::kTonal) {
    expect_near(measure::cents(pitch_at(x, 0.2, 0.2), midi_hz(60)), 0, 25, "cents", what + " plays C-4");
  } else if (kind == Kind::kNoise) {
    const double r = measure::autocorrelation(Span(x, 9600, 9600), static_cast<size_t>(std::lround(kRate / midi_hz(60))));
    expect(r < 0.5 || pitch_at(x, 0.2, 0.2) == 0.0, fmt("%s is noise, not a C-4 tone (period correlation %.2f)", what.c_str(), r));
  } else {
    const auto db = level_db(x);
    expect(db[1200] < -30, fmt("%s is percussive: gone while the key is still held (%.1f dB at 1.2 s)", what.c_str(), db[1200]));
  }
}

// How many MIDI channels each stack documents.
int stack_channels(int stack) {
  switch (stack) {
    case 18: return 5; case 19: return 4; case 20: return 4; case 21: return 10; case 31: return 6;
    case 32: return 16; case 33: return 9; case 34: return 4; case 35: return 6; case 36: return 16;
    case 41: return 5; case 43: return 6; case 45: return 2; default: return 0;
  }
}

// MIDI channel -> the voice each stack plays there.
int stack_voice(int stack, int channel) {
  switch (stack) {
    case 18: return std::array<int, 5>{0, 0, 1, 2, 9}[static_cast<size_t>(std::min(channel, 4))];
    case 19: return std::array<int, 4>{10, 10, 11, 12}[static_cast<size_t>(std::min(channel, 3))];
    case 20: return channel == 3 ? 14 : 13;
    case 21: return channel < 6 ? 17 : (channel < 9 ? 15 : 16);
    case 31: return channel < 3 ? 29 : 22;
    case 32: return channel < 6 ? 29 : (channel < 9 ? 22 : (channel < 15 ? 100 : 9));
    case 33: return channel < 8 ? 30 : 9;
    case 34: return (channel & 1) ? 25 : 24;
    case 35: return channel < 4 ? 26 : 37;
    case 36: return 28;
    case 41: return 40;
    case 43: return 42;
    case 45: return 44;
    default: return -1;
  }
}

void register_sources() {
  prove(P::kWaveform, "Waveform selects the named chip voice; stacks route MIDI channels to the chip's channels", [] {
    std::vector<std::vector<float>> solo(59);
    for (int w = 0; w < 59; ++w) {
      if (stack_voice(w, 0) >= 0) continue;
      solo[static_cast<size_t>(w)] = note({{P::kWaveform, static_cast<double>(w)}}, 1.5, 1.5).mono();
      expect_kind(solo[static_cast<size_t>(w)], kind_of(w), P::kWaveNames[w]);
    }
    // Distinct sources sound distinct. The plain square-wave chips (5B, SN76489,
    // AY, SAA1099, TIA at C-4) genuinely share a 50% square, and the SMS and
    // Genesis PSG are the same SN76489.
    auto same_family = [](int a, int b) {
      const std::array<int, 6> squares{8, 13, 15, 22, 42, 44};
      auto in = [&](int w) { return std::find(squares.begin(), squares.end(), w) != squares.end(); };
      return (in(a) && in(b)) || (a == 14 && b == 16) || (a == 16 && b == 14);
    };
    for (int a = 0; a < 59; ++a)
      for (int b = a + 1; b < 59; ++b) {
        if (solo[static_cast<size_t>(a)].empty() || solo[static_cast<size_t>(b)].empty() || same_family(a, b)) continue;
        if (kind_of(a) == Kind::kNoise && kind_of(b) == Kind::kNoise) continue;  // compared by their own proofs
        expect(timbre_similarity(solo[static_cast<size_t>(a)], solo[static_cast<size_t>(b)]) < 0.999,
               fmt("%s and %s sound different", P::kWaveNames[a], P::kWaveNames[b]));
      }
    // Stacks: every MIDI channel plays the chip channel the stack documents.
    for (int stack = 0; stack < 59; ++stack) {
      if (stack_voice(stack, 0) < 0) continue;
      for (int channel = 0; channel < stack_channels(stack); ++channel) {
        const int voice = stack_voice(stack, channel);
        if (channel > 0 && voice == stack_voice(stack, channel - 1) && channel != stack_channels(stack) - 1) continue;
        const auto x = render({{P::kWaveform, static_cast<double>(stack)}}, {on(0, 60, channel), off(1.5, 60, channel)}, 1.5).mono();
        expect_kind(x, kind_of(voice), fmt("%s channel %d (%s)", P::kWaveNames[stack], channel + 1, voice == 100 ? "rhythm" : P::kWaveNames[voice]));
      }
    }
  });
}

// ----- presets --------------------------------------------------------------------

enum Claim : unsigned {
  kSustain = 1, kDecay = 2, kBass = 4, kArp = 8, kEcho = 16, kChorus = 32, kRetroChain = 64,
  kSweep = 128, kNoisy = 256, kStack = 512, kCustom = 1024, kDutyMacro = 2048, kDrums = 4096,
};
struct PresetClaim { std::vector<int> voices; unsigned claims; };

// What each preset's name says, as the voice it must select and the character it must have.
PresetClaim preset_claim(int id) {
  static const std::array<PresetClaim, 82> table = {{
      {{}, 0},                          // 0 Manual
      {{0}, kSustain},                  // Clean NES lead
      {{0}, kSustain | kArp},           // NES chord lead
      {{9}, kDrums},                    // NES DPCM kit
      {{11}, 0},                        // Game Boy wave
      {{13}, kSustain | kBass},         // SMS bass
      {{17}, kDecay},                   // Genesis FM bell
      {{0}, kRetroChain},               // Bedroom CRT
      {{0}, kRetroChain},               // Noisy RF television
      {{26}, 0},                        // PC Engine glass
      {{27}, kSustain},                 // DOS OPL2 organ
      {{28}, kSustain},                 // OPL3 brass
      {{31}, 0},                        // PC-88 adventure
      {{32}, kDecay},                   // PC-98 FM piano
      {{33}, 0},                        // X68000 arcade
      {{25}, kSweep},                   // Atari POKEY zap
      {{38}, kBass},                    // SID 6581 bass
      {{39}, kSustain},                 // SID 8580 lead
      {{40}, kSustain},                 // Konami SCC lead
      {{42}, kArp | kDecay},            // Game Blaster bells
      {{46}, kSustain},                 // Vector wavetable pad
      {{47}, kSustain},                 // Phase-distortion brass
      {{48}, kSustain},                 // Additive drawbars
      {{49}, kDecay},                   // Six-operator electric piano
      {{50}, kSustain},                 // Digital partial strings
      {{8}, kBass},                     // Envelope bass trick
      {{0}, kArp},                      // Hyper arpeggio lead
      {{0}, kSustain},                  // Duty-cycle lead
      {{0}, kEcho},                     // Fake echo lead
      {{8}, kBass},                     // Octave power bass
      {{46}, kSustain | kChorus},       // Worn chorus pad
      {{3}, kSustain},                  // VRC6 heroic lead
      {{5}, kSustain},                  // FDS glass organ
      {{6}, kSustain | kChorus},        // N163 ensemble
      {{17}, kBass},                    // YM2612 growl bass
      {{30}, kDecay},                   // YM2151 arcade bell
      {{25}, kSweep},                   // POKEY metallic zap
      {{38}, kSustain},                 // SID combined reed
      {{9}, kDrums},                    // DPCM sixteen-key bank
      {{0}, kArp},                      // User-sequence spark
      {{28}, kRetroChain},              // Arcade CRT cabinet
      {{51}, kDecay},                   // Porta FM electric piano
      {{51}, kSustain},                 // Porta FM toy organ
      {{52}, kSustain | kChorus},       // Japanese analog poly
      {{53}, kSustain},                 // American matrix brass
      {{54}, kSustain | kChorus},       // Early sampler choir
      {{55}, kDecay},                   // Tine suitcase piano
      {{56}, kBass},                    // Classic ladder bass
      {{57}, kDrums},                   // Retro chip drum kit
      {{10}, kSweep},                   // Game Boy bubble bloop
      {{10}, kSweep},                   // Game Boy coin chirp
      {{12}, kSweep | kNoisy},          // Game Boy 7-bit zap
      {{11}, kDecay},                   // LSDJ wave pluck
      {{10}, kArp},                     // Game Boy fast chord
      {{10}, kEcho},                    // Game Boy tracker delay
      {{58}, kCustom | kSustain},       // Custom wave lead
      {{11}, kCustom | kBass},          // Game Boy custom bass
      {{58}, kCustom | kSustain},       // Custom wave organ
      {{10}, kDutyMacro},               // Game Boy duty macro
      {{1}, kBass},                     // NES triangle bass
      {{2}, kNoisy | kDecay},           // NES noise percussion
      {{4}, kSustain},                  // VRC6 saw lead
      {{7}, 0},                         // VRC7 hollow keys
      {{14}, kNoisy | kDecay},          // SMS noise percussion
      {{15}, kSustain},                 // Genesis PSG lead
      {{16}, kNoisy | kDecay},          // Genesis noise percussion
      {{18}, kStack},                   // NES channel stack
      {{19}, kStack},                   // Game Boy channel stack
      {{20}, kStack},                   // SMS channel stack
      {{21}, kStack},                   // Genesis channel stack
      {{22}, kSustain},                 // AY arcade lead
      {{23}, kNoisy | kDecay},          // AY noise percussion
      {{24}, kSustain},                 // POKEY pure lead
      {{29}, 0},                        // OPNA FM keys
      {{34}, kStack},                   // Atari channel stack
      {{35}, kStack},                   // PC Engine channel stack
      {{36}, kStack},                   // Sound Blaster channel stack
      {{37}, kNoisy | kDecay},          // PC Engine noise percussion
      {{41}, kStack},                   // SCC channel stack
      {{43}, kStack},                   // SAA1099 channel stack
      {{44}, kBass},                    // TIA pulse bass
      {{45}, kStack},                   // TIA channel stack
  }};
  return table[static_cast<size_t>(id)];
}

// Parameter values after selecting a preset, or after only selecting a voice.
std::vector<double> parameters_after(const Patch& patch) {
  Library library(g_clap_path);
  const clap_plugin_t* plugin = library.create();
  for (const auto& s : patch) set_param(plugin, s.id, s.value);
  std::vector<double> values(P::kParamCount);
  for (uint32_t id = 0; id < P::kParamCount; ++id) values[id] = param(plugin, id);
  plugin->destroy(plugin);
  return values;
}

// The scores an ingredient must be audible in (either will do): overlapping notes
// on one channel with different releases (Strict hardware, portamento, arpeggios,
// the stereo image), and short notes (release settings once an envelope is done).
// A stack plays every channel it documents.
std::vector<Score> preset_scores(int voice) {
  Score legato = {on(0.0, 60), on(0.4, 67), off(0.7, 60), off(2.2, 67)};
  Score short_notes = {on(0.0, 60), off(0.15, 60), on(0.6, 64), off(0.75, 64)};
  // One extra note per distinct kind of stack channel (enough to hear each chip
  // channel without exhausting the voice pool).
  for (int channel = 1; channel < stack_channels(voice); ++channel) {
    if (stack_voice(voice, channel) == stack_voice(voice, channel - 1)) continue;
    legato.push_back(on(0.1, 55, channel));
    legato.push_back(off(2.2, 55, channel));
    short_notes.push_back(on(0.0, 55, channel));
    short_notes.push_back(off(0.15, 55, channel));
  }
  return {legato, short_notes};
}

double take_difference_db(const Take& a, const Take& b) {
  double diff = 0, energy = 0;
  for (size_t i = 0; i < a.left.size(); ++i) {
    for (const auto [x, y] : {std::pair{a.left[i], b.left[i]}, std::pair{a.right[i], b.right[i]}}) {
      diff += (x - y) * (x - y);
      energy += x * x;
    }
  }
  return 10 * std::log10((diff + 1e-30) / (energy + 1e-30));
}

void prove_preset(int id) {
  const PresetClaim claim = preset_claim(id);
  const Patch patch = {{P::kPreset, static_cast<double>(id)}};
  const auto values = parameters_after(patch);
  const int voice = static_cast<int>(values[P::kWaveform]);
  const std::string name = P::kPresetNames[id];

  // 1. The chip or voice the name says.
  if (!claim.voices.empty())
    expect(std::find(claim.voices.begin(), claim.voices.end(), voice) != claim.voices.end(),
           fmt("plays %s (selected %s)", P::kWaveNames[claim.voices[0]], P::kWaveNames[voice]));
  if (claim.claims & kCustom)
    expect(values[P::kCustomWave] >= 0.5 || voice == 58, "plays the drawn custom wave");

  // 2. The character the name says, from a held C-4. Levels are window means
  // (a chorus or an arpeggio makes single points jitter).
  const Take held = render(patch, {on(0.0), off(1.5)}, 2.5);
  const auto x = held.mono();
  const double start = rms_db(x, 0.05, 0.25), later = rms_db(x, 1.0, 0.4);
  if (claim.claims & kSustain) expect(later - start > -6, fmt("sustains while held (%.1f dB after 1 s)", later - start));
  if (claim.claims & kDecay) expect(later - start < -10, fmt("decays while held, like its name (%.1f dB after 1 s)", later - start));
  if (claim.claims & kDrums) expect(later - start < -30, fmt("percussive (%.1f dB after 1 s)", later - start));
  if (claim.claims & kBass) {
    const double hz = pitch_at(x, 0.05, 0.25);
    expect(hz > 0 && hz < midi_hz(60 - 11.5), fmt("a bass: C-4 sounds an octave or more down (%.1f Hz)", hz));
  }
  if (claim.claims & kArp) {
    std::vector<int> pitches;
    for (double t = 0.02; t < 0.8; t += 0.01) {
      const double hz = pitch_at(x, t, 0.012);
      if (hz > 0) pitches.push_back(static_cast<int>(std::lround(12 * std::log2(hz / midi_hz(60)))));
    }
    std::sort(pitches.begin(), pitches.end());
    pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
    expect(pitches.size() >= 3, fmt("steps through a chord (%zu distinct pitches)", pitches.size()));
  }
  if (claim.claims & kSweep) {
    // Pitch (or, for noise, the spectral centroid) moves by a large step over the first 300 ms.
    const auto track = semitone_track(x, 0.005, 0.012);
    double lo = 1e9, hi = -1e9;
    for (size_t i = 2; i < 60 && i < track.size(); ++i) if (!std::isnan(track[i])) { lo = std::min(lo, track[i]); hi = std::max(hi, track[i]); }
    double clo = 1e9, chi = 0;
    for (double t = 0.005; t < 0.3; t += 0.01) {
      const double c = measure::spectral_centroid(Span(x, static_cast<size_t>(t * kRate), 1024), kRate, 10);
      if (c > 0) { clo = std::min(clo, c); chi = std::max(chi, c); }
    }
    expect(hi - lo >= 3.0 || chi / clo > 1.5, fmt("sweeps (pitch range %.1f semitones, centroid %.0f -> %.0f Hz)", hi > lo ? hi - lo : 0.0, clo, chi));
  }
  if (claim.claims & kNoisy) {
    const double r = measure::autocorrelation(Span(x, 2400, 4800), static_cast<size_t>(std::lround(kRate / midi_hz(60))));
    expect(r < 0.5 || pitch_at(x, 0.05, 0.1) == 0.0, fmt("noise, not a C-4 tone (period correlation %.2f)", r));
  }
  if (claim.claims & kEcho) {
    // A 30 ms note, then count the separate returns: rises back above -24 dB after dropping below -36.
    const auto e = level_db(render(patch, {on(0.0), off(0.03)}, 1.5).mono());
    int repeats = 0;
    bool low = false;
    for (size_t i = 40; i < e.size(); ++i) {
      if (e[i] < -36) low = true;
      else if (low && e[i] > -24) { ++repeats; low = false; }
    }
    expect(repeats >= 2, fmt("echoes after the note (%d repeats)", repeats));
  }
  if (claim.claims & kChorus) {
    const double lr = measure::correlation(Span(held.left, 9600, 24000), Span(held.right, 9600, 24000));
    expect(lr < 0.99, fmt("chorus widens it (left/right correlation %.3f)", lr));
  }
  if (claim.claims & kRetroChain) {
    const Take bypass = render(with(patch, {{P::kRetroAmount, 0}}), {on(0.0), off(1.5)}, 2.5);
    expect(take_difference_db(held, bypass) > -20, "the console/TV chain colours it");
  }
  if (claim.claims & kDutyMacro) {
    const auto d = duty_pattern(patch, 1.0 / values[P::kDutySeqRate], 4);
    expect(*std::max_element(d.begin(), d.end()) - *std::min_element(d.begin(), d.end()) > 0.1, "the duty changes as the note plays");
  }
  if (claim.claims & kStack) {
    const auto both = render(patch, {on(0, 60, 0), on(0, 67, 2), off(1.0, 60, 0), off(1.0, 67, 2)}, 1.0).mono();
    const Span body(both, 9600, 14400);
    const bool first = measure::tone_amplitude(body, kRate, midi_hz(60)) > 1e-3 || kind_of(stack_voice(voice, 0)) != Kind::kTonal;
    const bool third = measure::tone_amplitude(body, kRate, midi_hz(67)) > 1e-3 || kind_of(stack_voice(voice, 2)) != Kind::kTonal;
    expect(first && third, "channels 1 and 3 play at once, on their own chip channels");
  }

  // 3. Every setting the recipe makes (beyond picking the voice) is audible in it.
  const auto voice_only = parameters_after({{P::kWaveform, static_cast<double>(voice)}});
  const auto scores = preset_scores(voice);
  std::vector<Take> full;
  for (const auto& score : scores) full.push_back(render(patch, score, 2.6));
  for (uint32_t pid = 0; pid < P::kParamCount; ++pid) {
    if (pid == P::kPreset || pid == P::kWaveform || values[pid] == voice_only[pid]) continue;
    double d = -1e9;
    for (size_t k = 0; k < scores.size(); ++k)
      d = std::max(d, take_difference_db(full[k], render(with(patch, {{pid, voice_only[pid]}}), scores[k], 2.6)));
    expect(d > -40, fmt("its %s setting (%g, voice default %g) is audible (difference %.1f dB)", P::kSpecs[pid].name,
                        values[pid], voice_only[pid], d));
  }
}

void register_preset_parameter() {
  prove(P::kPreset, "Preset loads a complete recipe; Manual loads the default patch", [] {
    const auto defaults = parameters_after({});
    const auto manual = parameters_after({{P::kPreset, 12}, {P::kPreset, 0}});
    for (uint32_t id = 0; id < P::kParamCount; ++id)
      if (id != P::kPreset && id != P::kMasterDb && id != P::kPitchBendRange)
        expect(manual[id] == defaults[id], fmt("Manual restores %s to its default", P::kSpecs[id].name));
    // A preset is a complete recipe: nothing from the previous one survives.
    const auto after_echo = parameters_after({{P::kPreset, 28}, {P::kPreset, 1}});
    const auto direct = parameters_after({{P::kPreset, 1}});
    for (uint32_t id = 0; id < P::kParamCount; ++id)
      expect(after_echo[id] == direct[id] || id == P::kPreset, fmt("%s does not leak from the previous preset", P::kSpecs[id].name));
    // The host keeps its own master level and bend range across presets.
    const auto kept = parameters_after({{P::kMasterDb, -20}, {P::kPitchBendRange, 12}, {P::kPreset, 5}});
    expect(kept[P::kMasterDb] == -20 && kept[P::kPitchBendRange] == 12, "Master and Pitch bend range survive a preset change");
  });
}

}  // namespace

namespace {

void register_all() {
  register_level_and_pitch();
  register_envelope_and_glide();
  register_sequences();
  register_oscillators();
  register_fm();
  register_dpcm();
  register_effects();
  register_sources();
  register_preset_parameter();
}

int run_all(const char* filter) {
  register_all();
  int failed = 0, unproved = 0, ran = 0;
  for (uint32_t id = 0; id < P::kParamCount; ++id) {
    const auto& proof = param_proofs()[id];
    const char* name = P::kSpecs[id].name;
    if (!proof.run) {
      std::printf("NO PROOF  %s\n", name);
      ++unproved;
      continue;
    }
    if (filter && !std::strstr(name, filter)) continue;
    Outcome outcome{name, proof.claim, {}, 0};
    g_current = &outcome;
    proof.run();
    ++ran;
    if (outcome.failures.empty()) {
      std::printf("PASS      %-22s %s (%d checks)\n", name, proof.claim, outcome.checks);
    } else {
      ++failed;
      std::printf("FAIL      %-22s %s\n", name, proof.claim);
      for (const auto& f : outcome.failures) std::printf("            - %s\n", f.c_str());
    }
  }
  std::printf("\n%d parameters proved, %d failed, %d with no proof (of %u)\n\n", ran - failed, failed, unproved,
              static_cast<unsigned>(P::kParamCount));
  int preset_failed = 0, preset_ran = 0;
  constexpr int kPresets = static_cast<int>(std::size(P::kPresetNames));
  for (int id = 1; id < kPresets; ++id) {
    if (filter && !std::strstr(P::kPresetNames[id], filter)) continue;
    Outcome outcome{P::kPresetNames[id], "", {}, 0};
    g_current = &outcome;
    prove_preset(id);
    ++preset_ran;
    if (outcome.failures.empty()) {
      std::printf("PASS      preset %-28s (%d checks)\n", P::kPresetNames[id], outcome.checks);
    } else {
      ++preset_failed;
      std::printf("FAIL      preset %s\n", P::kPresetNames[id]);
      for (const auto& f : outcome.failures) std::printf("            - %s\n", f.c_str());
    }
  }
  std::printf("\n%d presets proved, %d failed (of %d)\n", preset_ran - preset_failed, preset_failed, kPresets - 1);
  return failed || unproved || preset_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s YANES.clap [name-filter]\n", argv[0]);
    return 2;
  }
  g_clap_path = argv[1];
  return run_all(argc >= 3 ? argv[2] : nullptr);
}
