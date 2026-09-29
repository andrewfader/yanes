// Cross-engine envelope-shape oracle.
//
// yanes-parity-compare scores timbre and pitch over a whole render; a note whose
// attack, decay or release is wrong can still correlate well there because the
// sustained body dominates. This oracle scores the amplitude envelope alone:
// both WAVs are reduced to a 5 ms-hop RMS contour in dB relative to their own
// peak, aligned by the lag that best explains the reference, and compared
// window by window, both as a whole-note mean and as the worst 50 ms stretch so
// that a short defect is not averaged away by a long sustain. A missing attack ramp, a release twice as long, a note that
// never releases, or an envelope that decays when it should hold all move the
// mean dB error or the note-end time far past what two correct engines differ by.
//
// Usage:
//   envelope_oracle <candidate.wav> <reference.wav> [options]
//     --level-db N   max |peak level difference| in dB          (default: not gated)
//     --shape-db N   max mean |envelope difference| in dB        (default 3)
//     --local-db N   max |envelope difference| over any 50 ms of
//                    the audible (> -25 dB) envelope              (default 6)
//     --end-ms N     max note-end (last window above -30 dB) gap (default 60)
//     --lag-ms N     max alignment lag searched and allowed      (default 60)
//     --window-ms N  RMS window; widen it for noise, whose LFSR can hold a
//                    value for tens of milliseconds                (default 20)
//   envelope_oracle --self-test
//
// The contour is floored at -40 dB below peak so the comparison is about the
// audible envelope, not the noise floor, and the two contours are compared up to
// one constant gain (the median dB offset over windows both sides hear), so an
// output-coupling transient that sets one engine's peak does not tilt the whole
// comparison. Level is only gated on request: two
// models of the same chip (SameBoy vs the Game Boy replay) should agree on it,
// but independent engines mix each chip at their own level, so across Furnace
// it is reported rather than scored.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Wav {
  uint32_t rate{};
  std::vector<double> mono;
};

inline uint32_t u32(const std::vector<uint8_t>& b, size_t p) {
  return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8) |
         (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24);
}
inline uint16_t u16(const std::vector<uint8_t>& b, size_t p) {
  return static_cast<uint16_t>(b[p]) | (static_cast<uint16_t>(b[p + 1]) << 8);
}

Wav load_wav(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
  Wav w;
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
    return w;
  uint16_t format = 0, bits = 0, chans = 0;
  size_t at = 0, n = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    const uint32_t z = u32(b, p + 4);
    if (p + 8 + z > b.size()) return w;
    if (!std::memcmp(b.data() + p, "fmt ", 4) && z >= 16) {
      format = u16(b, p + 8); chans = u16(b, p + 10);
      w.rate = u32(b, p + 12); bits = u16(b, p + 22);
    }
    if (!std::memcmp(b.data() + p, "data", 4)) { at = p + 8; n = z; }
    p += 8 + z + (z & 1);
  }
  if ((format != 1 && format != 3) || !chans || !w.rate || !at || !bits) {
    w.rate = 0;
    return w;
  }
  const size_t bytes = bits / 8;
  const size_t frames = n / (bytes * chans);
  w.mono.reserve(frames);
  for (size_t f = 0; f < frames; ++f) {
    double s = 0;
    for (uint16_t c = 0; c < chans; ++c) {
      const size_t p = at + (f * chans + c) * bytes;
      double v = 0;
      if (format == 3 && bits == 32) {
        float x; std::memcpy(&x, b.data() + p, 4); v = x;
      } else if (bits == 16) {
        v = static_cast<int16_t>(u16(b, p)) / 32768.0;
      } else if (bits == 24) {
        int32_t x = static_cast<int32_t>(b[p]) | (static_cast<int32_t>(b[p + 1]) << 8) |
                    (static_cast<int32_t>(b[p + 2]) << 16);
        if (x & 0x800000) x |= ~0xffffff;
        v = x / 8388608.0;
      } else {
        v = static_cast<int32_t>(u32(b, p)) / 2147483648.0;
      }
      s += v;
    }
    w.mono.push_back(s / chans);
  }
  return w;
}

constexpr double kHopSeconds = 0.005;
constexpr double kFloorDb = -40.0;
// The worst-stretch metric looks only at the clearly audible envelope. Below this
// an engine's output stage shows through (a unipolar chip DAC stepping to idle at
// key-off leaves a -30 dB coupling residual for ~50 ms) while every attack,
// decay or release defect the self-test plants is scored above it.
constexpr double kLocalFloorDb = -25.0;
constexpr double kEndDb = -30.0;

// RMS contour on a fixed 5 ms time grid, so two WAVs at different sample rates
// land on the same grid without resampling the audio. The DC component is
// removed per window: a Game Boy DAC idling at the bottom of its range, or an
// engine's output offset, is not an envelope.
// Every console AC-couples its output, and Furnace models it: when a note is cut
// mid-cycle the coupling capacitor bleeds the held level away over ~10 ms, while
// a DC-coupled render stops dead. Put both sides through the same 15 Hz coupling
// stage so an output-stage difference is not scored as an envelope one.
std::vector<double> ac_coupled(const Wav& w) {
  std::vector<double> out(w.mono.size());
  const double r = std::exp(-2.0 * 3.14159265358979323846 * 15.0 / w.rate);
  double x1 = 0, y1 = 0;
  for (size_t i = 0; i < w.mono.size(); ++i) {
    y1 = w.mono[i] - x1 + r * y1;
    x1 = w.mono[i];
    out[i] = y1;
  }
  return out;
}

std::vector<double> contour(const Wav& w, double window_seconds) {
  std::vector<double> out;
  const std::vector<double> x = ac_coupled(w);
  const size_t hop = std::max<size_t>(1, static_cast<size_t>(std::lround(w.rate * kHopSeconds)));
  const size_t win = std::max<size_t>(1, static_cast<size_t>(std::lround(w.rate * window_seconds)));
  for (size_t p = 0; p + win <= x.size(); p += hop) {
    double sum = 0, sq = 0;
    for (size_t i = 0; i < win; ++i) { sum += x[p + i]; sq += x[p + i] * x[p + i]; }
    const double mean = sum / static_cast<double>(win);
    out.push_back(std::sqrt(std::max(0.0, sq / static_cast<double>(win) - mean * mean)));
  }
  return out;
}

std::vector<double> to_db(const std::vector<double>& env, double peak) {
  std::vector<double> db(env.size());
  for (size_t i = 0; i < env.size(); ++i)
    db[i] = std::max(kFloorDb, 20.0 * std::log10(std::max(env[i], 1e-12) / peak));
  return db;
}

long last_above(const std::vector<double>& db, double threshold) {
  for (size_t i = db.size(); i > 0; --i) if (db[i - 1] > threshold) return static_cast<long>(i - 1);
  return -1;
}

struct Limits {
  double level_db = 1e9, shape_db = 3.0, local_db = 6.0, end_ms = 60.0, lag_ms = 60.0, window_ms = 20.0;
};

struct Verdict {
  bool pass = false;
  double level_db = 0, gain_db = 0, shape_db = 0, local_db = 0, end_ms = 0, lag_ms = 0;
  std::string reason;
};

Verdict compare(const Wav& candidate, const Wav& reference, const Limits& limits) {
  Verdict v;
  const double window = limits.window_ms / 1000.0;
  const auto c_env = contour(candidate, window), r_env = contour(reference, window);
  if (c_env.size() < 20 || r_env.size() < 20) { v.reason = "render shorter than 100 ms"; return v; }
  const double c_peak = *std::max_element(c_env.begin(), c_env.end());
  const double r_peak = *std::max_element(r_env.begin(), r_env.end());
  if (r_peak < 1e-4) { v.reason = "reference is silent (fixture problem, not evidence)"; return v; }
  if (c_peak < 1e-4) { v.reason = "candidate is silent"; return v; }
  v.level_db = 20.0 * std::log10(c_peak / r_peak);
  auto c = to_db(c_env, c_peak);
  const auto r = to_db(r_env, r_peak);

  // Per-window error with +/-10 ms of local timing slack: engines quantize note
  // events differently (Furnace acts on 60 Hz ticks, YANES on the event's sample),
  // and on a steep edge a few milliseconds is tens of dB. The slack is far below
  // any attack or release difference worth catching; the note-end check bounds
  // timing separately.
  constexpr long kSlack = 2;
  const long n = static_cast<long>(r.size());
  const long cn = static_cast<long>(c.size());
  auto errors_at = [&](long lag, long slack, double floor) {
    std::vector<double> e;
    for (long i = 0; i < n; ++i) {
      if (i + lag < 0 || i + lag >= cn) continue;
      double best_here = 1e30;
      bool active = false;
      for (long k = -slack; k <= slack; ++k) {
        const long j = i + lag + k;
        if (j < 0 || j >= cn) continue;
        const double cj = std::max(floor, c[static_cast<size_t>(j)]);
        const double ri = std::max(floor, r[static_cast<size_t>(i)]);
        active = active || cj > floor || ri > floor;
        best_here = std::min(best_here, std::abs(cj - ri));
      }
      if (active) e.push_back(best_here);
    }
    return e;
  };
  auto mean_of = [](const std::vector<double>& e) {
    double sum = 0;
    for (double x : e) sum += x;
    return e.empty() ? 1e30 : sum / static_cast<double>(e.size());
  };

  // Pick the lag (with no slack, so the estimate is sharp) that minimises the
  // mean error over windows either side hears.
  // A fixed engine latency is not an envelope defect; a lag at the search edge
  // means the envelopes do not line up at all and is reported as a failure.
  const long max_lag = static_cast<long>(std::lround(limits.lag_ms / 1000.0 / kHopSeconds));
  double best = 1e30;
  long best_lag = 0;
  for (long lag = -max_lag; lag <= max_lag; ++lag) {
    const double mean = mean_of(errors_at(lag, 0, kFloorDb));
    if (mean < best - 1e-9 || (std::abs(mean - best) <= 1e-9 && std::abs(lag) < std::abs(best_lag))) {
      best = mean;
      best_lag = lag;
    }
  }
  // Remove the one constant gain that best explains the difference (median over
  // windows both sides hear, well above the floor), then score what is left.
  std::vector<double> offsets;
  for (long i = 0; i < n; ++i) {
    const long j = i + best_lag;
    if (j < 0 || j >= cn) continue;
    const double cj = c[static_cast<size_t>(j)], ri = r[static_cast<size_t>(i)];
    if (cj > kFloorDb + 10.0 && ri > kFloorDb + 10.0) offsets.push_back(cj - ri);
  }
  if (!offsets.empty()) {
    std::nth_element(offsets.begin(), offsets.begin() + static_cast<long>(offsets.size() / 2), offsets.end());
    v.gain_db = offsets[offsets.size() / 2];
    for (double& x : c) x = std::max(kFloorDb, x - v.gain_db);
  }
  v.shape_db = mean_of(errors_at(best_lag, kSlack, kFloorDb));
  const std::vector<double> diff = errors_at(best_lag, kSlack, kLocalFloorDb);
  // Worst 50 ms stretch at that lag.
  constexpr size_t kLocal = 10;
  for (size_t i = 0; i + kLocal <= diff.size(); ++i) {
    double sum = 0;
    for (size_t k = 0; k < kLocal; ++k) sum += diff[i + k];
    v.local_db = std::max(v.local_db, sum / kLocal);
  }
  v.lag_ms = static_cast<double>(best_lag) * kHopSeconds * 1000.0;
  const long c_end = last_above(c, kEndDb) - best_lag, r_end = last_above(r, kEndDb);
  v.end_ms = static_cast<double>(c_end - r_end) * kHopSeconds * 1000.0;

  if (std::abs(v.level_db) > limits.level_db) v.reason = "peak level differs";
  else if (max_lag > 0 && std::abs(best_lag) == max_lag) v.reason = "envelopes do not align within the lag window";
  else if (v.shape_db > limits.shape_db) v.reason = "envelope shape differs";
  else if (v.local_db > limits.local_db) v.reason = "envelope differs locally (attack, decay or release)";
  else if (std::abs(v.end_ms) > limits.end_ms) v.reason = "note ends at a different time";
  else v.pass = true;
  return v;
}

void print(const Verdict& v, const char* label) {
  std::printf("%s: level=%+.2fdB gain=%+.2fdB shape=%.2fdB local=%.2fdB end=%+.0fms lag=%+.0fms result=%s%s%s\n",
              label, v.level_db, v.gain_db, v.shape_db, v.local_db, v.end_ms, v.lag_ms, v.pass ? "pass" : "fail",
              v.reason.empty() ? "" : " reason=", v.reason.c_str());
}

// ----- self-test: the oracle must reject the defects it exists to catch -------

// A 220 Hz square (or white noise) under a piecewise envelope: `attack` seconds
// up, a held level that falls `decay_db_per_s`, then `release` seconds down from
// `off`. `spike_db` adds a 10 ms onset overshoot, the shape an AC-coupled output
// gives a narrow pulse.
struct Note {
  uint32_t rate = 48000;
  double attack = 0.05, off = 1.0, release = 0.3, gain = 1.0, delay = 0.0;
  double decay_db_per_s = 0.0, spike_db = 0.0;
  bool noise = false;
  uint32_t seed = 1;
};

Wav synth(const Note& n) {
  Wav w;
  w.rate = n.rate;
  const size_t frames = static_cast<size_t>(n.rate * 2.0);
  w.mono.resize(frames);
  uint32_t lfsr = n.seed;
  for (size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / n.rate - n.delay;
    double env = 0;
    if (t >= 0) {
      env = n.attack > 0 ? std::min(1.0, t / n.attack) : 1.0;
      env *= std::pow(10.0, -n.decay_db_per_s * std::min(t, n.off) / 20.0);
      if (t < 0.01) env *= std::pow(10.0, n.spike_db / 20.0);
      if (t >= n.off) env *= n.release > 0 ? std::max(0.0, 1.0 - (t - n.off) / n.release) : 0.0;
    }
    lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
    const double osc = n.noise ? (static_cast<double>(lfsr) / 4294967295.0) * 2.0 - 1.0
                               : (std::fmod(t * 220.0 + 10.0, 1.0) < 0.5 ? 1.0 : -1.0);
    w.mono[i] = 0.5 * n.gain * env * osc;
  }
  return w;
}

int self_test() {
  auto with = [](auto edit) { Note n; edit(n); return n; };
  struct Case { const char* name; Note candidate, reference; bool expect; };
  const Case cases[] = {
      {"identical", Note{}, Note{}, true},
      {"quieter-same-shape", with([](Note& n) { n.gain = 0.5; }), Note{}, true},
      {"engine-latency", with([](Note& n) { n.delay = 0.02; }), Note{}, true},
      {"other-sample-rate", Note{}, with([](Note& n) { n.rate = 44100; }), true},
      {"noise-different-seed", with([](Note& n) { n.attack = 0.1; n.noise = true; n.seed = 7; }),
       with([](Note& n) { n.attack = 0.1; n.noise = true; n.seed = 99; }), true},
      {"coupling-onset-spike", Note{}, with([](Note& n) { n.spike_db = 4.0; }), true},
      {"attack-missing", with([](Note& n) { n.attack = 0.0; }), with([](Note& n) { n.attack = 0.4; }), false},
      {"attack-3x-slow", with([](Note& n) { n.attack = 0.3; }), with([](Note& n) { n.attack = 0.1; }), false},
      {"decay-missing", Note{}, with([](Note& n) { n.decay_db_per_s = 30.0; }), false},
      {"release-1.5x", with([](Note& n) { n.release = 0.45; }), Note{}, false},
      {"release-too-long", with([](Note& n) { n.release = 0.8; }), Note{}, false},
      {"release-ignored", with([](Note& n) { n.release = 0.0; }), with([](Note& n) { n.release = 0.5; }), false},
      {"note-never-ends", with([](Note& n) { n.off = 5.0; }), Note{}, false},
      {"level-way-off", with([](Note& n) { n.gain = 0.1; }), Note{}, false},
  };
  bool ok = true;
  for (const auto& c : cases) {
    Limits limits;
    limits.level_db = 12.0;
    const Verdict v = compare(synth(c.candidate), synth(c.reference), limits);
    const bool good = v.pass == c.expect;
    ok = ok && good;
    std::printf("self-test %-22s %s  ", c.name, good ? "ok " : "BAD");
    print(v, "");
  }
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return self_test();
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: envelope_oracle candidate.wav reference.wav "
                 "[--level-db N] [--shape-db N] [--local-db N] [--end-ms N] [--lag-ms N] [--window-ms N]\n"
                 "       envelope_oracle --self-test\n");
    return 2;
  }
  Limits limits;
  for (int i = 3; i < argc; ++i) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag.c_str()); return 2; }
    const double value = std::atof(argv[++i]);
    if (flag == "--level-db") limits.level_db = value;
    else if (flag == "--shape-db") limits.shape_db = value;
    else if (flag == "--local-db") limits.local_db = value;
    else if (flag == "--end-ms") limits.end_ms = value;
    else if (flag == "--lag-ms") limits.lag_ms = value;
    else if (flag == "--window-ms") limits.window_ms = value;
    else { std::fprintf(stderr, "unknown option %s\n", flag.c_str()); return 2; }
  }
  const Wav candidate = load_wav(argv[1]), reference = load_wav(argv[2]);
  if (!candidate.rate) { std::fprintf(stderr, "cannot read candidate WAV %s\n", argv[1]); return 1; }
  if (!reference.rate) { std::fprintf(stderr, "cannot read reference WAV %s\n", argv[2]); return 1; }
  const Verdict v = compare(candidate, reference, limits);
  print(v, "envelope");
  return v.pass ? 0 : 1;
}
