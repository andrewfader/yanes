// Preset volume leveling verification: renders every factory preset through the public
// CLAP interface across representative musical contexts, measuring peak and RMS levels,
// ensuring loudness uniformity when switching presets, and asserting headroom margin.

#include "clap_harness.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlockFrames = 512;

struct NoteMeasurement {
  int key{};
  float peak{0.0f};
  double note_on_rms{0.0};
  double total_rms{0.0};
  double peak_db{-120.0};
  double rms_db{-120.0};
};

struct PresetMeasurement {
  int id{};
  std::string name;
  bool is_drum{false};
  float max_peak{0.0f};
  double avg_rms{0.0};
  double peak_db{-120.0};
  double rms_db{-120.0};
  std::vector<NoteMeasurement> notes;
};

inline double to_db(double linear) {
  if (linear <= 1.0e-6) return -120.0;
  return 20.0 * std::log10(linear);
}

bool is_drum_preset(int id, const std::string& name) {
  return id == 3 || id == 38 || id == 48 ||
         name.find("drum") != std::string::npos || name.find("kit") != std::string::npos ||
         name.find("DPCM") != std::string::npos || name.find("percussion") != std::string::npos;
}

NoteMeasurement measure_note(Runner& runner, int key, double on_duration_sec, double off_duration_sec) {
  const int on_blocks = static_cast<int>(std::ceil(on_duration_sec * kSampleRate / kBlockFrames));
  const int off_blocks = static_cast<int>(std::ceil(off_duration_sec * kSampleRate / kBlockFrames));

  float peak = 0.0f;
  double on_sum_sq = 0.0;
  size_t on_sample_count = 0;
  double total_sum_sq = 0.0;
  size_t total_sample_count = 0;

  // Trigger Note On
  Events on_events;
  on_events.push(note_event(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(key), 1, 1.0));
  Block b = runner.run(&on_events);
  peak = std::max(peak, b.peak);
  for (uint32_t f = 0; f < runner.frames(); ++f) {
    float l = runner.left()[f], r = runner.right()[f];
    on_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
    total_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
  }
  on_sample_count += runner.frames() * 2;
  total_sample_count += runner.frames() * 2;

  // Sustain portion
  for (int i = 1; i < on_blocks; ++i) {
    b = runner.run();
    peak = std::max(peak, b.peak);
    for (uint32_t f = 0; f < runner.frames(); ++f) {
      float l = runner.left()[f], r = runner.right()[f];
      on_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
      total_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
    }
    on_sample_count += runner.frames() * 2;
    total_sample_count += runner.frames() * 2;
  }

  // Trigger Note Off
  Events off_events;
  off_events.push(note_event(CLAP_EVENT_NOTE_OFF, 0, static_cast<int16_t>(key), 1, 0.0));
  b = runner.run(&off_events);
  peak = std::max(peak, b.peak);
  for (uint32_t f = 0; f < runner.frames(); ++f) {
    float l = runner.left()[f], r = runner.right()[f];
    total_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
  }
  total_sample_count += runner.frames() * 2;

  // Release portion
  for (int i = 1; i < off_blocks; ++i) {
    b = runner.run();
    peak = std::max(peak, b.peak);
    for (uint32_t f = 0; f < runner.frames(); ++f) {
      float l = runner.left()[f], r = runner.right()[f];
      total_sum_sq += static_cast<double>(l) * l + static_cast<double>(r) * r;
    }
    total_sample_count += runner.frames() * 2;
  }

  NoteMeasurement result;
  result.key = key;
  result.peak = peak;
  result.note_on_rms = on_sample_count > 0 ? std::sqrt(on_sum_sq / static_cast<double>(on_sample_count)) : 0.0;
  result.total_rms = total_sample_count > 0 ? std::sqrt(total_sum_sq / static_cast<double>(total_sample_count)) : 0.0;
  result.peak_db = to_db(result.peak);
  result.rms_db = to_db(result.note_on_rms);
  return result;
}

PresetMeasurement measure_preset(const clap_plugin_t* plugin, int preset_id, const std::string& name) {
  clap_id preset_param = find_param(plugin, "Preset");
  set_param(plugin, preset_param, static_cast<double>(preset_id));

  Runner runner(plugin, kSampleRate, kBlockFrames);
  runner.settle(8);

  const bool drum = is_drum_preset(preset_id, name);
  std::vector<int> test_keys = drum ? std::vector<int>{36, 48, 60} : std::vector<int>{48, 60, 72};

  PresetMeasurement pm;
  pm.id = preset_id;
  pm.name = name;
  pm.is_drum = drum;

  double sum_rms = 0.0;
  for (int key : test_keys) {
    runner.settle(4);
    NoteMeasurement nm = measure_note(runner, key, 0.6, 0.3);
    pm.notes.push_back(nm);
    pm.max_peak = std::max(pm.max_peak, nm.peak);
    sum_rms += nm.note_on_rms;
  }
  if (preset_id == 16) {
    // The bass must remain audible below the general preset suite's C3-C5 range.
    for (const int key : {36, 43}) {
      runner.settle(4);
      const auto bass = measure_note(runner, key, 0.6, 0.3);
      assert(bass.rms_db > -24.0 && "SID bass must retain level in the low register");
      assert(bass.peak_db < -3.0 && "SID bass must retain headroom");
    }
  }
  pm.avg_rms = sum_rms / static_cast<double>(test_keys.size());
  pm.peak_db = to_db(pm.max_peak);
  pm.rms_db = to_db(pm.avg_rms);
  return pm;
}

// 1. Verify loudness limits & adequate headroom on every preset
void test_preset_loudness_bounds(const std::vector<PresetMeasurement>& measurements) {
  std::printf("Running test_preset_loudness_bounds...\n");
  for (const auto& pm : measurements) {
    // Assert no preset clips or exceeds -3.0 dBFS peak headroom limit
    assert(pm.peak_db <= -3.0 && "Preset peak must have at least 3 dB headroom");
    assert(pm.max_peak < 0.999f && "Preset must not clip");

    if (pm.is_drum) {
      // Drum presets must have punchy peaks between -12.0 and -3.0 dBFS
      assert(pm.peak_db >= -12.0 && "Drum preset peak must be at least -12 dBFS");
      assert(pm.rms_db >= -26.0 && "Drum preset RMS must be audible");
    } else {
      // Tonal presets must stay within the balanced range: RMS between -21.0 and -14.0 dBFS
      assert(pm.rms_db >= -21.0 && "Tonal preset must not be too quiet (< -21 dBFS RMS)");
      assert(pm.rms_db <= -14.0 && "Tonal preset must not be too loud (> -14 dBFS RMS)");
    }
  }
  std::printf("  PASS: All %zu presets satisfy peak headroom (<= -3.0 dBFS) and loudness balance.\n",
              measurements.size());
}

// 2. Verify smooth transitions: consecutive presets do not have jarring volume swings
void test_preset_switching_smoothness(const std::vector<PresetMeasurement>& measurements) {
  std::printf("Running test_preset_switching_smoothness...\n");
  double max_step_delta = 0.0;
  int max_step_from = -1;

  for (size_t i = 0; i + 1 < measurements.size(); ++i) {
    const auto& p1 = measurements[i];
    const auto& p2 = measurements[i + 1];
    // Compare tonal-to-tonal transitions
    if (!p1.is_drum && !p2.is_drum) {
      const double delta = std::abs(p1.rms_db - p2.rms_db);
      if (delta > max_step_delta) {
        max_step_delta = delta;
        max_step_from = p1.id;
      }
      // Consecutive tonal presets must be within 5.5 dB of each other
      assert(delta <= 5.5 && "Loudness jump between consecutive presets must not exceed 5.5 dB");
    }
  }
  std::printf("  PASS: Max consecutive tonal loudness delta is %.2f dB (between #%d and #%d), within 5.5 dB threshold.\n",
              max_step_delta, max_step_from, max_step_from + 1);
}

// 3. Verify determinism: switching away and back reproduces identical audio levels
void test_preset_switching_determinism(const clap_plugin_t* plugin, const std::vector<PresetMeasurement>& reference) {
  std::printf("Running test_preset_switching_determinism...\n");
  const clap_id preset_id = find_param(plugin, "Preset");

  // Pick diverse presets: quietest, loudest, middle, drum
  const int test_indices[] = {1, 3, 7, 8, 12, 29, 43, 47, 54};
  for (int idx : test_indices) {
    const auto& ref = reference[static_cast<size_t>(idx - 1)];
    // Switch to another preset first (e.g. 5)
    set_param(plugin, preset_id, 5.0);
    // Now re-measure target preset
    PresetMeasurement pm = measure_preset(plugin, idx, ref.name);

    const double rms_diff = std::abs(pm.rms_db - ref.rms_db);
    const double peak_diff = std::abs(pm.peak_db - ref.peak_db);
    // Modulated effects (chorus LFO, phasing, detune) introduce slight natural phase variance (~1 dB).
    assert(rms_diff < 1.5 && "Preset RMS must be deterministic when switching presets");
    assert(peak_diff < 2.5 && "Preset peak must be deterministic when switching presets");
  }
  std::printf("  PASS: Preset level reproducibility confirmed across varied switching paths.\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <path_to_yanes.clap>\n", argv[0]);
    return 1;
  }
  const Library library(argv[1]);
  const clap_plugin_t* plugin = library.create();
  assert(plugin);

  const clap_id preset_id = find_param(plugin, "Preset");
  const clap_param_info_t preset_info = param_info(plugin, preset_id);
  const int num_presets = static_cast<int>(preset_info.max_value);

  std::vector<bool> covered(static_cast<size_t>(param_info(plugin, find_param(plugin, "Waveform")).max_value) + 1, false);
  std::vector<PresetMeasurement> measurements;
  measurements.reserve(static_cast<size_t>(num_presets));

  std::printf("=========================================================================================\n");
  std::printf("                     YANES PRESET VOLUME MEASUREMENTS (TOTAL: %d)\n", num_presets);
  std::printf("=========================================================================================\n");
  std::printf("%-4s | %-28s | %-10s | %-11s | %-10s | %-11s | %-8s\n",
              "ID", "Preset Name", "Peak (lin)", "Peak (dBFS)", "RMS (lin)", "RMS (dBFS)", "Headroom");
  std::printf("-----+------------------------------+------------+-------------+------------+-------------+---------\n");

  for (int i = 1; i <= num_presets; ++i) {
    char name_buf[128]{};
    params_of(plugin)->value_to_text(plugin, preset_id, static_cast<double>(i), name_buf, sizeof(name_buf));
    std::string name = name_buf;

    PresetMeasurement pm = measure_preset(plugin, i, name);
    measurements.push_back(pm);
    covered[static_cast<size_t>(param(plugin, find_param(plugin, "Waveform")))] = true;

    const double headroom = -pm.peak_db;
    std::printf("%-4d | %-28s | %10.4f | %9.2f dB | %10.4f | %9.2f dB | %6.2f dB\n",
                pm.id, pm.name.c_str(),
                static_cast<double>(pm.max_peak), pm.peak_db,
                pm.avg_rms, pm.rms_db, headroom);
  }

  std::printf("=========================================================================================\n");

  // Summary statistics
  double min_rms_db = 100.0, max_rms_db = -200.0;
  double min_peak_db = 100.0, max_peak_db = -200.0;
  int min_rms_idx = -1, max_rms_idx = -1;
  int clipping_count = 0;

  for (const auto& pm : measurements) {
    if (pm.rms_db < min_rms_db) { min_rms_db = pm.rms_db; min_rms_idx = pm.id; }
    if (pm.rms_db > max_rms_db) { max_rms_db = pm.rms_db; max_rms_idx = pm.id; }
    if (pm.peak_db < min_peak_db) { min_peak_db = pm.peak_db; }
    if (pm.peak_db > max_peak_db) { max_peak_db = pm.peak_db; }
    if (pm.max_peak >= 0.999f) clipping_count++;
  }

  std::printf("Summary Statistics:\n");
  std::printf("  Quietest Preset: #%d (%s) at RMS = %.2f dBFS\n",
              min_rms_idx, measurements[static_cast<size_t>(min_rms_idx - 1)].name.c_str(), min_rms_db);
  std::printf("  Loudest Preset:  #%d (%s) at RMS = %.2f dBFS\n",
              max_rms_idx, measurements[static_cast<size_t>(max_rms_idx - 1)].name.c_str(), max_rms_db);
  std::printf("  RMS Dynamic Range Across Presets: %.2f dB (from %.2f dBFS to %.2f dBFS)\n",
              max_rms_db - min_rms_db, min_rms_db, max_rms_db);
  std::printf("  Peak Range Across Presets:        %.2f dBFS to %.2f dBFS\n",
              min_peak_db, max_peak_db);
  std::printf("  Presets near clipping (>= -0.01 dBFS): %d\n", clipping_count);
  std::printf("=========================================================================================\n");

  for (bool source : covered) assert(source && "Every source needs a factory starting preset");

  // Run automated test assertions
  test_preset_loudness_bounds(measurements);
  test_preset_switching_smoothness(measurements);
  test_preset_switching_determinism(plugin, measurements);

  std::printf("=========================================================================================\n");
  std::printf("ALL PRESET VOLUME TESTS PASSED.\n");
  std::printf("=========================================================================================\n");

  plugin->destroy(plugin);
  return 0;
}
