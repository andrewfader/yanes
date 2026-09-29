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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
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

// Amplitude envelope with 1 ms resolution: peak |x| per millisecond.
std::vector<double> envelope_ms(const std::vector<float>& x) {
  const size_t hop = static_cast<size_t>(kRate / 1000.0);
  std::vector<double> e;
  for (size_t p = 0; p + hop <= x.size(); p += hop) e.push_back(measure::peak(Span(x, p, hop)));
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

// Pitch in semitones relative to MIDI note 60 every `hop` seconds.
std::vector<double> semitone_track(const std::vector<float>& x, double hop, double window, double fmax = 5000.0) {
  std::vector<double> out;
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
  // Collapse detections within 8 ms of each other (the window straddling a step).
  std::vector<double> merged;
  for (double t : times) if (merged.empty() || t - merged.back() > 0.008) merged.push_back(t);
  return merged;
}

double median_interval(const std::vector<double>& t) {
  std::vector<double> d;
  for (size_t i = 1; i < t.size(); ++i) d.push_back(t[i] - t[i - 1]);
  if (d.empty()) return 0;
  std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
  return d[d.size() / 2];
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
      expect_near(time_to_fall(env, 0.01, held, 500) - 500.0, 0.99 * r, 2.0 + 0.03 * r, "ms",
                  fmt("Release %.0f ms (linear voice), time to -40 dB", r));
    }
    // Chip envelope generators (VRC7, SID) release at a constant dB rate: -60 dB at the release time.
    for (double r : {300.0, 900.0}) {
      const auto env = envelope_ms(note({{P::kWaveform, 7}, {P::kFmSustainRate, 0}, {P::kAttackMs, 0},
                                         {P::kReleaseMs, r}}, 0.5, 0.5 + r / 1000 + 0.2).mono());
      const double held = *std::max_element(env.begin() + 400, env.begin() + 490);
      expect_near(time_to_fall(env, 0.1, held, 500) - 500.0, r / 3.0, 3.0 + 0.05 * r, "ms",
                  fmt("Release %.0f ms (VRC7 envelope generator), time to -20 dB", r));
      expect_near(time_to_fall(env, 0.01, held, 500) - 500.0, 2.0 * r / 3.0, 3.0 + 0.05 * r, "ms",
                  fmt("Release %.0f ms (VRC7 envelope generator), time to -40 dB", r));
    }
  });
  prove(P::kPortamentoMs, "Portamento glides between notes with that time constant", [] {
    for (double glide : {0.0, 60.0, 250.0}) {
      const auto x = render(with(kClean, {{P::kPortamentoMs, glide}}),
                            {on(0.0, 60), off(0.5, 60), on(0.5, 72), off(1.4, 72)}, 1.4).mono();
      const auto track = semitone_track(x, 0.001, 0.008);
      // Exponential glide: 63.2% of the 12-semitone interval after one time constant.
      double t63 = -1;
      for (size_t i = 500; i < track.size(); ++i)
        if (!std::isnan(track[i]) && track[i] >= 12.0 * (1.0 - std::exp(-1.0))) { t63 = static_cast<double>(i) - 500.0; break; }
      expect_near(t63, glide, 6.0 + 0.08 * glide, "ms", fmt("Portamento %.0f ms, time to 63%% of the interval", glide));
      expect_near(track[1300], 12.0, 0.1, "semitones", fmt("Portamento %.0f ms arrives at the new note", glide));
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
      expect_near(onset, delay + 4.8, 6.0, "ms", fmt("Vibrato delay %.0f ms: onset of the wobble", delay));
    }
  });
}

}  // namespace

namespace {

void register_all() {
  register_level_and_pitch();
  register_envelope_and_glide();
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
  std::printf("\n%d parameters proved, %d failed, %d with no proof (of %u)\n", ran - failed, failed, unproved,
              static_cast<unsigned>(P::kParamCount));
  return failed || unproved ? 1 : 0;
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
