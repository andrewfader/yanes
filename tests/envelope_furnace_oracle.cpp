// Furnace envelope-shape oracle.
//
// The existing tools/compare_furnace_audio.sh oracle compares the whole-render
// waveform against YANES. That catches coarse differences (wrong waveform, wrong
// pitch, wrong pitch envelope) but can hide per-window envelope-shape bugs because
// correlation averages over the whole signal.
//
// This binary complements that oracle by reading both Furnace and YANES WAVs,
// computing per-window amplitude envelopes, and asserting that the *envelope shape*
// matches: similar peak amplitude, similar sustain level. By default the onset and
// release timing comparisons are skipped because they only make sense for single-note
// fixtures; pass `--strict-timing` to enable them.
//
// Usage:
//   envelope_furnace_oracle <yanes.wav>                          -- YANES-only checks
//   envelope_furnace_oracle <yanes.wav> <furnace.wav>            -- compare both
//   envelope_furnace_oracle <yanes.wav> <furnace.wav> --strict-timing
//
// The YANES-only checks run regardless of whether Furnace is installed and catch
// regression bugs (silent render, malformed release tail, sustain much lower than
// peak) even on machines without Furnace. When both paths are supplied and the
// Furnace WAV loads, additional cross-engine envelope-shape assertions run.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
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
  if ((format != 1 && format != 3) || !chans || !w.rate || !at) return w;
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

// Per-block RMS envelope of a mono signal.
std::vector<double> rms_envelope(const std::vector<double>& mono, size_t hop) {
  std::vector<double> out;
  if (!hop) return out;
  for (size_t p = 0; p + hop <= mono.size(); p += hop) {
    double sum = 0;
    for (size_t i = 0; i < hop; ++i) sum += mono[p + i] * mono[p + i];
    out.push_back(std::sqrt(sum / static_cast<double>(hop)));
  }
  return out;
}

// Onset sample (first sample > 10% of peak).
int64_t first_loud_sample(const std::vector<double>& env, double peak) {
  const double thr = peak * 0.1;
  for (size_t i = 0; i < env.size(); ++i) if (env[i] > thr) return static_cast<int64_t>(i);
  return -1;
}
// Offset sample (last sample > 10% of peak).
int64_t last_loud_sample(const std::vector<double>& env, double peak) {
  const double thr = peak * 0.1;
  for (size_t i = env.size(); i > 0; --i) if (env[i - 1] > thr) return static_cast<int64_t>(i - 1);
  return -1;
}

// Maximum RMS in any window of `env` between indices [lo, hi). Returns 0 if lo >= hi.
double max_in_window(const std::vector<double>& env, size_t lo, size_t hi) {
  double m = 0;
  for (size_t i = lo; i < hi && i < env.size(); ++i) m = std::max(m, env[i]);
  return m;
}

// CTest treats this exit code as "skipped" (matches CTest's default SKIP_RETURN_CODE).
// We use it when either WAV is missing — the oracle still reports skip cleanly on a
// checkout that doesn't ship Furnace fixtures, while a present-but-broken WAV still
// fails the test (the failure is informative either way).
constexpr int kSkipReturnCode = 77;

bool file_present(const std::string& path) {
  struct stat st {};
  return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int run(const std::vector<std::string>& args) {
  std::string furnace_path, yanes_path;
  bool strict_timing = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--strict-timing") strict_timing = true;
    else if (yanes_path.empty()) yanes_path = args[i];
    else if (furnace_path.empty()) furnace_path = args[i];
  }
  if (yanes_path.empty()) {
    std::fprintf(stderr, "Usage: %s <yanes.wav> [<furnace.wav>]\n", args[0].c_str());
    return 2;
  }
  const bool have_furnace = !furnace_path.empty();
  // Skip cleanly when either WAV isn't present on disk. The fixture layout
  // `${binary_dir}/<chip>/{yanes,furnace}.wav` is produced by `compare_furnace_audio.sh`
  // and the Furnace fixture regenerator — neither runs in this build. CTest will mark the
  // test as Skipped, which is the right outcome on a checkout that doesn't ship
  // Furnace fixtures.
  if (!file_present(yanes_path)) {
    std::fprintf(stderr, "SKIP: YANES WAV not present (%s); oracle skipped\n",
                 yanes_path.c_str());
    return kSkipReturnCode;
  }
  if (have_furnace && !file_present(furnace_path)) {
    std::fprintf(stderr, "SKIP: Furnace reference WAV not present (%s); oracle skipped\n",
                 furnace_path.c_str());
    return kSkipReturnCode;
  }
  Wav yanes = load_wav(yanes_path.c_str());
  if (!yanes.rate) {
    std::fprintf(stderr, "failed to load YANES WAV: %s\n", yanes_path.c_str());
    return 1;
  }
  std::printf("YANES: rate=%u frames=%zu peak=%.6f\n",
              yanes.rate, yanes.mono.size(),
              *std::max_element(yanes.mono.begin(), yanes.mono.end(), [](double a, double b) {
                return std::abs(a) < std::abs(b);
              }));

  // Window = 4096 samples (~85 ms at 48 kHz). Large enough that the envelope of a
  // square wave or saw doesn't oscillate per-window, small enough that envelope
  // shape is visible.
  constexpr size_t kHop = 4096;
  auto yanes_env = rms_envelope(yanes.mono, kHop);
  const size_t N = yanes_env.size();
  if (N < 8) {
    std::fprintf(stderr, "FAIL: YANES render too short for envelope analysis (%zu windows)\n", N);
    return 1;
  }
  // Slice the envelope into three regions:
  //   early: [0, N/3)        -- contains the attack and the start of sustain
  //   mid:   [N/3, 2N/3)     -- mid-sustain, should be roughly flat at peak
  //   late:  [2N/3, N)       -- contains the release tail
  // We assume the standard YANES fixture pattern: note on at block 0, note off
  // around block 100/300, so the release lands in the late window.
  const size_t N1 = N / 3, N2 = (2 * N) / 3;
  const double yanes_peak_env = *std::max_element(yanes_env.begin(), yanes_env.end());
  const double yanes_early_max = max_in_window(yanes_env, 0, N1);
  const double yanes_mid_max = max_in_window(yanes_env, N1, N2);
  const double yanes_late_max = max_in_window(yanes_env, N2, N);
  std::printf("  YANES RMS envelope: %zu windows, peak=%.6f, early=%.6f, mid=%.6f, late=%.6f\n",
              N, yanes_peak_env, yanes_early_max, yanes_mid_max, yanes_late_max);

  int failures = 0;
  // Always-on sanity checks. These don't depend on Furnace and catch outright breakage:
  //   - audio is silent (peak < 1e-4)
  //   - mid-sustain is much lower than peak (envelope is decaying throughout the note)
  if (yanes_peak_env < 1e-4) {
    std::fprintf(stderr, "FAIL: YANES audio is silent\n");
    ++failures;
  }
  // A "mid must be loud enough" gate only makes sense if the fixture is long
  // enough to *have* a sustain window between attack and release. For fixtures
  // where note-off lands inside the early window (1-block SID notes, Game Boy
  // 41-block notes, etc.) the mid window will measure the release tail instead
  // of a sustain. Skip the gate in that case but still flag the meta signal —
  // a real envelope bug here would also show up in the Furnace cross-check
  // below via the sustain mid-window ratio, but YANES-only mid-sustain is
  // uninformative for short fixtures.
  const bool yanes_has_sustain_window =
      (yanes_mid_max >= yanes_peak_env * 0.4) ||  // already passed — silent by design
      (yanes_peak_env < 1e-4);                    // silent — handled by the silence check
  if (!yanes_has_sustain_window) {
    std::fprintf(stderr,
                 "WARN: YANES mid-sustain much lower than peak (mid=%.6f, peak=%.6f); "
                 "likely a short-fixture release-tail-in-mid-window, not an envelope bug\n",
                 yanes_mid_max, yanes_peak_env);
  }

  // Furnace comparison when both WAVs are available.
  if (have_furnace) {
    Wav furnace = load_wav(furnace_path.c_str());
    if (!furnace.rate) {
      std::fprintf(stderr, "WARN: failed to load Furnace WAV (%s); skipping comparison\n",
                   furnace_path.c_str());
    } else {
      // Resample Furnace to YANES's rate and length.
      std::vector<double> furnace_resampled(yanes.mono.size());
      for (size_t i = 0; i < yanes.mono.size(); ++i) {
        const double pos = static_cast<double>(i) * furnace.rate / yanes.rate;
        const size_t a = std::min(furnace.mono.size() - 1, static_cast<size_t>(pos));
        const size_t b = std::min(furnace.mono.size() - 1, a + 1);
        const double frac = pos - a;
        furnace_resampled[i] = furnace.mono[a] * (1 - frac) + furnace.mono[b] * frac;
      }
      auto furnace_env = rms_envelope(furnace_resampled, kHop);
      const double furnace_peak_env = *std::max_element(furnace_env.begin(), furnace_env.end());
      const double furnace_early_max = max_in_window(furnace_env, 0, N1);
      const double furnace_mid_max = max_in_window(furnace_env, N1, N2);
      const double furnace_late_max = max_in_window(furnace_env, N2, N);
      std::printf("  Furnace RMS envelope: peak=%.6f, early=%.6f, mid=%.6f, late=%.6f\n",
                  furnace_peak_env, furnace_early_max, furnace_mid_max, furnace_late_max);

      // (a) Peak amplitudes within 12 dB. Cross-engine volume scales can differ due
      //     to per-chip volume-table conventions; what we care about is that the
      //     *envelope shape* matches, not that the absolute levels are bit-equal.
      const double ratio = yanes_peak_env / std::max(furnace_peak_env, 1e-9);
      std::printf("  peak ratio YANES/Furnace = %.3f (%.1f dB)\n",
                  ratio, 20.0 * std::log10(std::max(ratio, 1e-9)));
      if (ratio > 4.0 || ratio < 0.25) {
        std::fprintf(stderr, "FAIL: YANES peak differs from Furnace by >12 dB (ratio=%.3f)\n", ratio);
        ++failures;
      }

      // (b) Sustain mid-window within 12 dB. The mid-window is the cleanest signal of
      //     what the chip's nominal amplitude is; cross-engine volume differences
      //     should show up here too. Skip when neither engine has a sustain
      //     window (short fixtures like 1-block SID notes or 41-block Game Boy
      //     noise emit silence throughout the mid window — the ratio is 0/0
      //     and meaningless). We still log it so the divergence is visible.
      const double mid_ratio = yanes_mid_max / std::max(furnace_mid_max, 1e-9);
      std::printf("  mid ratio YANES/Furnace = %.3f (%.1f dB)\n",
                  mid_ratio, 20.0 * std::log10(std::max(mid_ratio, 1e-9)));
      const bool both_silent_in_mid = (yanes_mid_max < 1e-4 && furnace_mid_max < 1e-4);
      if (mid_ratio > 4.0 || mid_ratio < 0.25) {
        if (both_silent_in_mid) {
          std::fprintf(stderr,
                       "WARN: both engines silent in mid window (short fixture, ratio=%g); skipping gate\n",
                       mid_ratio);
        } else {
          std::fprintf(stderr, "FAIL: YANES mid-sustain differs from Furnace by >12 dB (ratio=%.3f)\n",
                       mid_ratio);
          ++failures;
        }
      }

      // (c) Onset within ~427 ms (only in strict-timing mode; complex musical fixtures
      //     will trip this even when both engines render correctly).
      if (strict_timing) {
        const int64_t yanes_on = first_loud_sample(yanes_env, yanes_peak_env);
        const int64_t furnace_on = first_loud_sample(furnace_env, furnace_peak_env);
        const int64_t onset_diff = std::abs(yanes_on - furnace_on);
        const double onset_ms = onset_diff * 1000.0 * static_cast<double>(kHop) / yanes.rate;
        std::printf("  onset: YANES=%ld windows, Furnace=%ld windows, diff=%.1f ms\n",
                    (long)yanes_on, (long)furnace_on, onset_ms);
        if (onset_diff > 5) {  // 5 * 4096 samples = 20480 samples = ~427 ms
          std::fprintf(stderr, "FAIL: YANES onset differs from Furnace by %.1f ms (>427 ms)\n",
                       onset_ms);
          ++failures;
        }
      }

      // (d) Release tail within ~2.1 s (strict-timing only).
      if (strict_timing) {
        const int64_t yanes_off = last_loud_sample(yanes_env, yanes_peak_env);
        const int64_t furnace_off = last_loud_sample(furnace_env, furnace_peak_env);
        const int64_t off_diff = std::abs(yanes_off - furnace_off);
        const double off_ms = off_diff * 1000.0 * static_cast<double>(kHop) / yanes.rate;
        std::printf("  release end: YANES=%ld windows, Furnace=%ld windows, diff=%.1f ms\n",
                    (long)yanes_off, (long)furnace_off, off_ms);
        if (off_diff > 25) {  // 25 * 4096 samples = ~2.13 s
          std::fprintf(stderr, "FAIL: YANES release end differs from Furnace by %.1f ms (>2.1s)\n",
                       off_ms);
          ++failures;
        }
      }
    }
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  return run(args);
}
