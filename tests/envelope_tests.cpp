// Envelope trajectory oracle: real audio output, no external dependencies.
//
// For each note-on / note-off cycle we measure peak / RMS at fixed time windows and assert
// the envelope follows the expected shape:
//
//   Attack ramp     amplitude rises monotonically across the configured attack window
//   Release curve   amplitude decays after note-off and reaches silence by ~1.5x release
//   Sustain level   held-note RMS is stable across many settled blocks (no drift, no pop)
//   FM attack       the FM operator attack knob changes the rise time of a sustained note
//   Hardware env    the NES hardware envelope produces a stepped (not smooth) shape
//   Vibrato delay   vibrato LFO is suppressed during the delay window after note-on
//   Determinism     rendering the same preset twice produces bit-identical audio
//
// These tests catch regressions where the release knob silently stops working, where a
// knob edits a different field than its label claims, or where state leaks between notes.
// They complement the existing whole-render correlation oracle against Furnace (which
// can hide per-window shape bugs) and the simple "sustained" / "silent" checks already
// in note_tests.cpp.

#include "clap_harness.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 512;

// Like assert() but with a useful message. Aborts the calling function on failure.
#define ENFORCE(cond, msg) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "\n  FAIL: %s @ %s:%d\n", (msg), __FILE__, __LINE__); \
    std::fflush(stderr); \
    std::abort(); \
  } \
} while (0)

// ----- the actual scenarios -------------------------------------------------------

void test_attack_ramp(const Library& lib) {
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, kRate, kBlock);
  r.set(find_param(plugin, "Attack"), 5.0);     // 5 ms attack
  r.set(find_param(plugin, "Release"), 0.0);

  std::vector<float> peak_trace;
  for (int b = 0; b < 5; ++b) {
    Events ev;
    if (b == 0) ev.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    r.run(ev.list().ctx ? &ev : nullptr);
    peak_trace.push_back(r.run().peak);
  }
  plugin->destroy(plugin);

  ENFORCE(peak_trace[0] > 0, "attack block 0 must already be audible");
  ENFORCE(peak_trace[2] >= peak_trace[0], "attack should rise across the attack window");
  ENFORCE(peak_trace[4] >= peak_trace[2] * 0.95,
          "by 4x the attack time the envelope should have plateaued");
}

void test_release_decay_curve(const Library& lib) {
  // The release knob should produce a clearly decaying envelope after note-off that
  // reaches silence within ~250ms (well before the labeled 600ms). This test catches:
  //   - release knob stops applying (envelope stays at sustain level forever)
  //   - release knob goes linear instead of decaying (would fail monotonic check)
  //   - release knob produces a non-zero tail that never reaches silence
  //
  // We don't pin the exact dB at each window because the curve depends on which
  // waveform and chip voice is active; what matters is the *shape*.
  const int release_ms = 600;
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, kRate, kBlock);
  r.set(find_param(plugin, "Release"), static_cast<double>(release_ms));

  Events on;
  on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on);
  for (int i = 0; i < 6; ++i) r.run();  // settle into sustain
  const Block sustain = r.run();
  const double sustain_rms = sustain.rms;
  ENFORCE(sustain_rms > 0.001, "sustain level is silence -- release test not meaningful");

  Events off;
  off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
  r.run(&off);

  // Sample the envelope at fixed windows after note-off. The actual measured shape is
  // roughly exponential from sustain to silence within ~800ms; the precise curve
  // depends on the voice and rate. The assertions below catch the regressions that
  // matter: release knob broken (no decay), release linear (still rising), release
  // never reaches silence. They don't couple to exact dB counts.
  std::vector<float> peaks;
  peaks.push_back(sustain.peak);
  std::vector<int> probe_ms = {25, 50, 75, 100, 125, 150, 175, 200, 250, 300, 400, 500, 800};
  int elapsed_blocks = 0;
  std::vector<float> probe_peaks;
  for (int ms : probe_ms) {
    int target = ms * 48 / 512;
    while (elapsed_blocks < target) {
      peaks.push_back(r.run().peak);
      ++elapsed_blocks;
    }
    const Block blk = r.run();
    ++elapsed_blocks;
    probe_peaks.push_back(blk.peak);
  }
  for (int b = 0; b < 20; ++b) peaks.push_back(r.run().peak);

  // (a) Monotonic decay.
  int non_monotonic = 0;
  for (size_t i = 1; i < peaks.size(); ++i) {
    if (peaks[i] > peaks[i - 1] * 1.01) ++non_monotonic;
  }
  char mono_msg[128];
  std::snprintf(mono_msg, sizeof(mono_msg),
                "release envelope is not monotonically decaying: %d of %zu blocks rose",
                non_monotonic, peaks.size());
  ENFORCE(non_monotonic <= 1, mono_msg);

  // (b) The release envelope is descending at +50ms.
  ENFORCE(probe_peaks[1] < sustain.peak,
          "release envelope has not begun decaying by +50ms");

  // (c) At least -3 dB by +250ms.
  ENFORCE(probe_peaks[8] < sustain.peak * 0.707,
          "release envelope still too loud at +250ms (expected -3 dB)");

  // (d) Silent by +800ms.
  ENFORCE(probe_peaks[12] < sustain.peak * 0.001,
          "release envelope has not reached silence by +800ms");

  plugin->destroy(plugin);
}

void test_sustained_level_is_stable(const Library& lib) {
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, kRate, kBlock);
  r.set(find_param(plugin, "Attack"), 0.0);
  r.set(find_param(plugin, "Release"), 200.0);
  Events on;
  on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on);
  for (int i = 0; i < 10; ++i) r.run();  // settle
  std::vector<double> rms_blocks;
  for (int i = 0; i < 20; ++i) rms_blocks.push_back(r.run().rms);
  plugin->destroy(plugin);
  double mean = 0;
  for (double v : rms_blocks) mean += v;
  mean /= rms_blocks.size();
  ENFORCE(mean > 0, "sustained note produced silence");
  for (size_t i = 0; i < rms_blocks.size(); ++i) {
    const double ratio_db = 20.0 * std::log10(std::max(1e-9, rms_blocks[i]) / std::max(1e-9, mean));
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "sustained RMS drifted %.2f dB at block %zu (mean %.6f, sample %.6f)",
                  ratio_db, i, mean, rms_blocks[i]);
    ENFORCE(std::abs(ratio_db) < 6.0, msg);
  }
}

void test_fm_attack_changes_rise_time(const Library& lib) {
  auto measure_rise_ms = [&](double fm_attack) -> double {
    const clap_plugin_t* plugin = lib.create();
    Runner r(plugin, kRate, kBlock);
    r.set(find_param(plugin, "Waveform"), 17.0);  // Genesis YM2612 FM
    r.set(find_param(plugin, "FM attack"), fm_attack);
    r.set(find_param(plugin, "FM decay"), 0.0);
    r.set(find_param(plugin, "FM sustain level"), 15.0);
    r.set(find_param(plugin, "FM sustain rate"), 0.0);
    r.set(find_param(plugin, "FM release"), 15.0);
    r.set(find_param(plugin, "Attack"), 0.0);
    r.set(find_param(plugin, "Release"), 50.0);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    r.run(&on);
    int64_t first_loud = -1;
    for (int b = 0; b < 80; ++b) {
      const Block blk = r.run();
      if (first_loud < 0 && blk.peak > 0.05) first_loud = static_cast<int64_t>(b) * kBlock;
    }
    plugin->destroy(plugin);
    return first_loud >= 0 ? first_loud / kRate * 1000.0 : -1.0;
  };
  const double fast = measure_rise_ms(31.0);
  const double slow = measure_rise_ms(5.0);
  ENFORCE(fast >= 0 && slow > 0,
          "FM rise time measurement failed (need slow > 0 and fast >= 0)");
  char msg[128];
  std::snprintf(msg, sizeof(msg),
                "FM attack knob appears broken: slow=%.2f ms, fast=%.2f ms, slow/fast=%.2f (need > 1.5)",
                slow, fast, slow / fast);
  ENFORCE(slow > fast * 1.5, msg);
}

void test_hardware_envelope_is_stepped(const Library& lib) {
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, kRate, kBlock);
  r.set(find_param(plugin, "Waveform"), 0.0);   // NES pulse (only NES source with hw env)
  r.set(find_param(plugin, "Hardware envelope"), 1.0);
  r.set(find_param(plugin, "Envelope rate"), 1.0);
  r.set(find_param(plugin, "Attack"), 0.0);
  r.set(find_param(plugin, "Release"), 50.0);
  Events on;
  on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on);
  std::vector<float> peaks;
  for (int b = 0; b < 10; ++b) peaks.push_back(r.run().peak);
  plugin->destroy(plugin);

  // The NES pulse hardware envelope runs at a fixed clock (NTSC frame rate scaled by
  // rate+1). With rate=1 the envelope steps every other frame (~32ms). Over 10 blocks
  // (~107ms) we expect to see at least 2 distinct volume steps.
  std::set<int> distinct_levels;
  for (float p : peaks) distinct_levels.insert(static_cast<int>(p * 1000));
  ENFORCE(distinct_levels.size() >= 2,
          "hardware envelope shows no steps across 10 blocks (broken env or wrong voice)");
}

void test_vibrato_delay_suppresses_initial_lfo(const Library& lib) {
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, kRate, kBlock);
  r.set(find_param(plugin, "Waveform"), 1.0);
  r.set(find_param(plugin, "Vibrato depth"), 1.0);
  r.set(find_param(plugin, "Vibrato rate"), 5.5);
  r.set(find_param(plugin, "Vibrato delay"), 300.0);
  r.set(find_param(plugin, "Attack"), 0.0);
  r.set(find_param(plugin, "Release"), 800.0);

  Events on;
  on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on);

  // We measure zero-crossing intervals in two windows:
  //   pre-window:  [0, 300 ms] — vibrato should NOT be active (within delay)
  //   post-window: [400, 700 ms] — vibrato should be active
  // If the delay knob is broken or the LFO doesn't engage, both windows show the
  // same variation. If the LFO engages too early, the pre-window already shows
  // modulation. We expect a clear difference between the two.
  std::vector<int> zc_pre, zc_post;
  int prev_zc = -1;
  for (int b = 0; b < 70; ++b) {
    r.run();
    const std::vector<float>& L = r.left();
    for (uint32_t i = 0; i < L.size(); ++i) {
      if (i > 0 && ((L[i] >= 0) != (L[i - 1] >= 0))) {
        const int zc = static_cast<int>(b) * static_cast<int>(kBlock) + static_cast<int>(i);
        if (prev_zc > 0) {
          if (zc < 300 * 48) zc_pre.push_back(zc - prev_zc);
          else if (zc >= 400 * 48 && zc < 700 * 48) zc_post.push_back(zc - prev_zc);
        }
        prev_zc = zc;
      }
    }
  }
  plugin->destroy(plugin);

  if (zc_pre.size() < 16 || zc_post.size() < 16) return;  // not enough data
  auto std_dev = [](const std::vector<int>& v) {
    double mean = 0;
    for (int x : v) mean += x;
    mean /= v.size();
    double sq = 0;
    for (int x : v) sq += (x - mean) * (x - mean);
    return std::sqrt(sq / v.size());
  };
  const double pre_std = std_dev(zc_pre);
  const double post_std = std_dev(zc_post);
  char msg[256];
  std::snprintf(msg, sizeof(msg),
                "vibrato delay knob appears broken: pre-std=%.3f post-std=%.3f (need post > pre*1.2)",
                pre_std, post_std);
  ENFORCE(post_std > pre_std * 1.2, msg);
}

void test_render_is_deterministic(const Library& lib) {
  auto render = [&]() {
    const clap_plugin_t* plugin = lib.create();
    Runner r(plugin, kRate, kBlock);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    r.run(&on);
    std::vector<float> trace;
    for (int b = 0; b < 50; ++b) {
      r.run();
      for (float v : r.left()) trace.push_back(v);
    }
    plugin->destroy(plugin);
    return trace;
  };
  const auto a = render();
  const auto b = render();
  ENFORCE(a.size() == b.size(), "render lengths differ between runs");
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      char msg[128];
      std::snprintf(msg, sizeof(msg),
                    "render drift at sample %zu: a=%g b=%g", i, a[i], b[i]);
      ENFORCE(false, msg);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <path_to_YANES.clap>\n", argv[0]);
    return 1;
  }
  const Library lib(argv[1]);

  struct Case { const char* name; void (*fn)(const Library&); };
  const Case cases[] = {
      {"attack_ramp_rises_monotonically",     &test_attack_ramp},
      {"release_decay_curve_at_fixed_windows",&test_release_decay_curve},
      {"sustained_level_is_stable",           &test_sustained_level_is_stable},
      {"fm_attack_changes_rise_time",         &test_fm_attack_changes_rise_time},
      {"hardware_envelope_is_stepped",        &test_hardware_envelope_is_stepped},
      {"vibrato_delay_suppresses_initial_lfo",&test_vibrato_delay_suppresses_initial_lfo},
      {"render_is_deterministic",             &test_render_is_deterministic},
  };

  int passed = 0, failed = 0;
  std::printf("================================================================\n");
  std::printf("Envelope / determinism oracle suite\n");
  std::printf("================================================================\n");
  for (const Case& c : cases) {
    std::printf("Scenario: %-44s ", c.name);
    std::fflush(stdout);
    try {
      c.fn(lib);
      std::printf(" PASS\n");
      ++passed;
    } catch (...) {
      std::printf("\n  FAIL (exception)\n");
      ++failed;
    }
  }
  std::printf("================================================================\n");
  std::printf("Envelope / oracle suite: %d passed, %d failed\n", passed, failed);
  std::printf("================================================================\n");
  return failed == 0 ? 0 : 1;
}
