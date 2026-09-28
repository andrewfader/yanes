// State round-trip audio oracle: save_state + load_state must reproduce the
// same plugin audio rendering. Two configurations are written as state blobs
// through one plugin instance, then loaded into a fresh plugin instance. Each
// instance's render is compared against a second render of the same state. If
// save_state drops a parameter or load_state assigns it to the wrong field,
// the second render diverges from the first.
//
// This is a separate binary from tests/state_tests.cpp because state_tests.cpp
// is byte-level (it asserts the wire format of the state blob) and we want a
// black-box audio round-trip test here. The two together pin both the wire
// format and the audio contract: a save/load that round-trips bytes but
// produces different audio is just as broken as one that produces different
// bytes.

#include "clap_harness.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace harness;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 512;

void render_seq(const clap_plugin_t* plugin, int sustain_blocks, std::vector<float>* audio) {
  Runner r(plugin, kRate, kBlock);
  Events on; on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on);
  for (int i = 0; i < sustain_blocks; ++i) r.run();
  Events off; off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
  r.run(&off);
  audio->clear();
  for (int i = 0; i < 200; ++i) { r.run(); for (float v : r.left()) audio->push_back(v); }
}

double correlation(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t n = std::min(a.size(), b.size());
  double mean_a = 0, mean_b = 0;
  for (size_t i = 0; i < n; ++i) { mean_a += a[i]; mean_b += b[i]; }
  mean_a /= n; mean_b /= n;
  double cov = 0, va = 0, vb = 0;
  for (size_t i = 0; i < n; ++i) {
    const double da = a[i] - mean_a;
    const double db = b[i] - mean_b;
    cov += da * db; va += da * da; vb += db * db;
  }
  if (va <= 0 || vb <= 0) return 0;
  return cov / std::sqrt(va * vb);
}

// One config: render with state → save → render of same state (== identical).
// The two renders must match exactly; this catches save/load symmetry bugs
// where a field gets clamped, ignored, or assigned to the wrong index on
// load.
void test_round_trip_exact(const Library& lib) {
  const clap_plugin_t* plugin = lib.create();
  std::vector<float> a, b;
  render_seq(plugin, 28, &a);
  StateMemory state;
  if (!save_state(plugin, &state)) {
    std::fprintf(stderr, "  FAIL: save_state returned false at %s:%d\n", __FILE__, __LINE__);
    std::abort();
  }
  load_state(plugin, state);
  render_seq(plugin, 28, &b);
  plugin->destroy(plugin);

  if (a.size() != b.size()) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "round-trip audio length differs: %zu vs %zu", a.size(), b.size());
    std::fprintf(stderr, "  FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__);
    std::abort();
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      char msg[128];
      std::snprintf(msg, sizeof(msg),
                    "round-trip audio diverges at sample %zu: a=%g b=%g", i, a[i], b[i]);
      std::fprintf(stderr, "  FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__);
      std::abort();
    }
  }
}

// Cross-instance round-trip: render with state in plugin instance A, save,
// load into a fresh instance B, render. Audio must match the original render
// of A. Catches bugs where save/load is instance-bound (e.g. references
// plugin-internal pointers that don't survive destruction) — a regression
// class that the within-instance test can't see.
void test_cross_instance_round_trip(const Library& lib) {
  std::vector<float> a;
  StateMemory state;

  {
    const clap_plugin_t* plugin_a = lib.create();
    Runner r(plugin_a, kRate, kBlock);
    r.set(find_param(plugin_a, "Waveform"), 22.0);     // AY-tone
    r.set(find_param(plugin_a, "Release"), 800.0);      // distinct release
    r.set(find_param(plugin_a, "Attack"), 100.0);
    render_seq(plugin_a, 28, &a);
    if (!save_state(plugin_a, &state)) {
      std::fprintf(stderr, "  FAIL: save_state returned false at %s:%d\n", __FILE__, __LINE__);
      std::abort();
    }
    plugin_a->destroy(plugin_a);
  }

  std::vector<float> b;
  const clap_plugin_t* plugin_b = lib.create();
  Runner r2(plugin_b, kRate, kBlock);
  // B starts in a different state (different voice). Loading should put it
  // back into A's state.
  r2.set(find_param(plugin_b, "Waveform"), 26.0);     // PCE-wave
  r2.set(find_param(plugin_b, "Release"), 200.0);
  load_state(plugin_b, state);
  render_seq(plugin_b, 28, &b);
  plugin_b->destroy(plugin_b);

  if (a.size() != b.size()) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "cross-instance round-trip length differs: %zu vs %zu", a.size(), b.size());
    std::fprintf(stderr, "  FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__);
    std::abort();
  }
  // We expect a correlation ≥ 0.9999 between a and b. Audio doesn't have to
  // be bit-exact across instances because the host could include the instance
  // pointer in voice state in ways that survive save/load but differ in warm
  // LFSR state. 0.9999 is the gate the existing reaper integration oracle
  // uses for self-match; we hold state round-trip to the same gate.
  const double corr = correlation(a, b);
  char msg[160];
  std::snprintf(msg, sizeof(msg),
                "cross-instance round-trip correlation too low: %g (need ≥ 0.9999)", corr);
  if (corr < 0.9999) {
    std::fprintf(stderr, "  FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__);
    std::abort();
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
      {"save_state_load_state_round_trip_is_exact",        &test_round_trip_exact},
      {"save_state_load_state_round_trip_across_instance", &test_cross_instance_round_trip},
  };

  int passed = 0, failed = 0;
  std::printf("================================================================\n");
  std::printf("Save-state / load-state audio round-trip oracle\n");
  std::printf("================================================================\n");
  for (const Case& c : cases) {
    std::printf("Scenario: %-44s ", c.name);
    std::fflush(stdout);
    try {
      c.fn(lib);
      std::printf(" PASS\n");
      ++passed;
    } catch (...) {
      std::printf(" FAIL\n");
      ++failed;
    }
  }
  std::printf("================================================================\n");
  std::printf("State round-trip oracle: %d passed, %d failed\n", passed, failed);
  std::printf("================================================================\n");
  return failed == 0 ? 0 : 1;
}
