// SNES S-DSP parity: the plug-in's clean-room SNES sampler (BRR decode, the four
// predictor filters, the 4-point Gaussian interpolation and pitch-tracked
// resampling) must match the hardware-reference S-DSP for the same BRR sample
// played at the same pitch. The reference (blargg's SPC_DSP, as used by
// cycle-accurate emulators) is rendered in snes_dsp_reference.cpp, kept in its
// own translation unit so its NDEBUG define does not disable this test's checks.
#include "clap_harness.hpp"
#include "platform_test.hpp"
#include "../src/snes_dsp.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// Defined in snes_dsp_reference.cpp (links the reference S-DSP).
std::vector<float> snes_reference_render(const uint8_t* blocks, int block_bytes, int loop_block, int pitch, int frames);

using namespace harness;
namespace fs = std::filesystem;

namespace {

// Render the same sample through YANES's SNES voice at `key` (base key `base`),
// at the S-DSP's native rate so the two are directly comparable.
std::vector<float> yanes_render(const Library& library, const std::string& brr_path, int base, int key, int frames) {
  const bool ok = test_platform::set_environment("YANES_DPCM_BANK", brr_path);
  assert(ok);
  const clap_plugin_t* plugin = library.create();
  test_platform::unset_environment("YANES_DPCM_BANK");
  std::vector<float> out;
  {
    Runner runner(plugin, static_cast<double>(yanes::snes::kNativeRate), 256);
    runner.set(find_param(plugin, "Waveform"), 63);
    runner.set(find_param(plugin, "DPCM base key"), base);
    runner.set(find_param(plugin, "DPCM loop mask"), 1);
    runner.set(find_param(plugin, "Attack"), 0);
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

// Best normalised cross-correlation of two level-normalised signals over a steady
// window, searching a small lag to absorb the voices' different key-on delays.
double best_correlation(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t from = 2000, len = 6000;  // skip onset, compare the sustain
  const double ra = rms(a, from, from + len), rb = rms(b, from, from + len);
  if (ra < 1e-6 || rb < 1e-6) return 0.0;
  double best = -1.0;
  for (int lag = -128; lag <= 128; ++lag) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < len; ++i) {
      const size_t ia = from + i; const long ib = static_cast<long>(from + i) + lag;
      if (ib < 0 || static_cast<size_t>(ib) >= b.size()) continue;
      const double va = a[ia] / ra, vb = b[static_cast<size_t>(ib)] / rb;
      dot += va * vb; na += va * va; nb += vb * vb;
    }
    if (na > 0 && nb > 0) best = std::max(best, dot / std::sqrt(na * nb));
  }
  return best;
}

// Play `blocks` (whose loop point is loop_block) through both engines across a
// sweep of pitches and return how many fell below the 0.9 correlation gate; the
// .brr file at brr_path is what YANES imports.
int run_parity(const Library& library, const std::vector<uint8_t>& blocks, int loop_block,
               const std::string& brr_path, const char* label, double& worst) {
  const int base = 84, frames = 12000;
  int failures = 0;
  std::printf("%s:\n", label);
  for (const int key : {72, 67, 60, 76}) {
    const double ratio = std::exp2((key - base) / 12.0);               // YANES playback ratio
    const int pitch = static_cast<int>(std::lround(ratio * 4096.0));   // matching S-DSP pitch
    const auto reference = snes_reference_render(blocks.data(), static_cast<int>(blocks.size()), loop_block, pitch, frames);
    const auto mine = yanes_render(library, brr_path, base, key, frames);
    const double corr = best_correlation(reference, mine);
    std::printf("  key %2d  ratio %.4f  pitch 0x%04X  correlation %.4f\n", key, ratio, pitch, corr);
    if (!(corr > 0.9)) ++failures;
    worst = std::min(worst, corr);
  }
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: snes_dsp_parity <clap>\n"); return 2; }
  const Library library(argv[1]);
  int failures = 0;
  double worst = 1.0;

  // A clean looping sine, encoded to BRR, drives both engines from identical bytes.
  std::vector<int16_t> sine(2048);
  for (size_t i = 0; i < sine.size(); ++i)
    sine[i] = static_cast<int16_t>(std::lround(11000.0 * std::sin(6.28318530718 * i / 128.0)));
  const auto synth = yanes::snes::brr_encode(sine.data(), sine.size(), true);
  const fs::path dir = fs::temp_directory_path() / ("yanes-snes-parity-" + std::to_string(test_platform::process_id()));
  fs::create_directories(dir);
  const std::string synth_path = (dir / "tone.brr").string();
  { std::ofstream f(synth_path, std::ios::binary); f.put(0); f.put(0); f.write(reinterpret_cast<const char*>(synth.data()), static_cast<std::streamsize>(synth.size())); }
  failures += run_parity(library, synth, 0, synth_path, "synthetic sine", worst);

  // Real cartridge .brr samples (e.g. from a legal backup), when supplied, are
  // pinned to the hardware reference too. Skipped when the variable is unset.
  if (const char* env = std::getenv("YANES_SNES_SAMPLES"); env && *env) {
    const std::string list = env;
    const char sep = test_platform::path_list_separator();
    int tested = 0;
    for (size_t start = 0; start <= list.size() && tested < 4;) {
      const size_t end = list.find(sep, start);
      const std::string path = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
      start = end == std::string::npos ? list.size() + 1 : end + 1;
      if (path.empty()) continue;
      std::ifstream f(path, std::ios::binary);
      std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
      if (file.size() < 11 || (file.size() - 2) % 9 != 0) continue;  // expect a looped .brr with header
      const int loop_block = (file[0] | (file[1] << 8)) / 9;
      const std::vector<uint8_t> blocks(file.begin() + 2, file.end());
      failures += run_parity(library, blocks, loop_block, path, path.c_str(), worst);
      ++tested;
    }
  }

  fs::remove_all(dir);
  if (failures) {
    std::fprintf(stderr, "SNES S-DSP parity FAILED: %d pitch(es) below 0.9 correlation (worst %.4f)\n", failures, worst);
    return 1;
  }
  std::printf("SNES S-DSP parity: all pitches matched the hardware reference (worst correlation %.4f)\n", worst);
  return 0;
}
