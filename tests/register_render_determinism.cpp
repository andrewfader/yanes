// Register-render determinism oracle.
//
// yanes-register-render takes a register script (`sample reg value` per line)
// and writes a WAV. We assert the same script produces audible, bit-identical
// output across two invocations. This catches non-determinism introduced by
// uninitialized chip state or run-time random seeding in the low-level chip
// emulator that the register-render binary uses directly (VGM playback,
// register scripts), a different surface than tests/state_roundtrip_tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Each chip is driven by the same tone script its *_register_render CTest uses,
// so the stimulus actually keys a voice on that chip. A script the chip ignores
// renders silence twice, and two silences are trivially bit-identical.
std::string fixture_for(const std::string& chip) {
  if (chip == "ym2151" || chip == "ym3812" || chip == "ymf262") return chip + "_tone.reg";
  return "ym2612_tone.reg";  // OPN family: YM2612, YM2203 and YM2608 share the OPN map.
}

// Read a single 16-bit signed PCM WAV into a mono float vector, summing channels
// if needed (the register-render writes stereo; we collapse to mono for diff).
std::vector<float> load_wav_mono(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
  if (b.size() < 44) return {};
  auto u16 = [&](size_t p) { return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8); };
  auto u32 = [&](size_t p) { return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8)
                              | (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24); };
  if (std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return {};
  // The determinism oracle only needs channels + bits + data chunk to fold the
  // stereo register-render output to mono. Sample rate and format (PCM vs float)
  // are part of the WAV header but not used here — kept out of the parse so the
  // -Wunused-but-set-variable stays silent and the parser stays minimal.
  uint16_t channels = 0, bits = 0;
  uint32_t data_offset = 0, data_len = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    const uint32_t chunk_size = u32(p + 4);
    if (std::memcmp(b.data() + p, "fmt ", 4) == 0 && chunk_size >= 16) {
      channels = u16(p + 10); bits = u16(p + 22);
    }
    if (std::memcmp(b.data() + p, "data", 4) == 0) {
      data_offset = p + 8;
      data_len = chunk_size;
      break;
    }
    p += 8 + chunk_size + (chunk_size & 1);
  }
  if (!data_offset || !data_len || bits != 16) return {};
  const size_t frames = data_len / (channels * 2);
  std::vector<float> out;
  out.reserve(frames);
  for (size_t i = 0; i < frames; ++i) {
    int32_t left = 0, right = 0;
    auto read_one = [&](size_t chan) {
      const size_t p = data_offset + (i * channels + chan) * 2;
      const int16_t v = static_cast<int16_t>(b[p] | (b[p + 1] << 8));
      return static_cast<int32_t>(v);
    };
    left = read_one(0);
    if (channels == 2) right = read_one(1);
    out.push_back(static_cast<float>(left + right) / 65536.0f);
  }
  return out;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
  const size_t n = std::min(a.size(), b.size());
  double m = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double d = std::abs(a[i] - b[i]);
    if (d > m) m = d;
  }
  return m;
}

double peak(const std::vector<float>& a) {
  double m = 0.0;
  for (float v : a) m = std::max(m, static_cast<double>(std::abs(v)));
  return m;
}

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "FAIL: %s\n", message.c_str());
  std::exit(1);
}

void run_case(const std::string& chip, const std::string& renderer, const fs::path& data_dir,
              const fs::path& work_dir) {
  const fs::path script = data_dir / fixture_for(chip);
  const fs::path wav1 = work_dir / (chip + "_determinism_1.wav");
  const fs::path wav2 = work_dir / (chip + "_determinism_2.wav");
  for (const fs::path& wav : {wav1, wav2}) {
    std::string cmd = "\"" + renderer + "\" " + chip + " \"" + script.string() + "\" \"" +
                      wav.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd /c strips the outermost pair of quotes.
#endif
    if (std::system(cmd.c_str()) != 0) fail("yanes-register-render failed for " + chip);
  }
  const std::vector<float> a = load_wav_mono(wav1.string());
  const std::vector<float> b = load_wav_mono(wav2.string());
  if (a.empty()) fail(chip + " render is not a readable 16-bit WAV");
  if (peak(a) < 1e-3) fail(chip + " render is silent; determinism of silence proves nothing");
  if (a.size() != b.size())
    fail(chip + " render length differs across two runs (" + std::to_string(a.size()) + " vs " +
         std::to_string(b.size()) + ")");
  const double mad = max_abs_diff(a, b);
  if (mad != 0.0) fail(chip + " render is not bit-identical between two runs (max abs diff " +
                       std::to_string(mad) + ")");
  std::printf("  %-8s render is audible (peak %.3f) and bit-identical across two runs\n",
              chip.c_str(), peak(a));
  fs::remove(wav1);
  fs::remove(wav2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "Usage: %s <yanes-register-render> <tests/data dir> <work dir>\n", argv[0]);
    return 2;
  }
  std::printf("yanes-register-render determinism oracle\n");
  // Each chip is an independent code path inside the emulator; drift in any one fails.
  for (const char* chip : {"ym2612", "ym2203", "ym2608", "ym2151", "ym3812", "ymf262"})
    run_case(chip, argv[1], argv[2], argv[3]);
  std::printf("Register-render determinism: PASS\n");
  return 0;
}
