// Register-render determinism oracle.
//
// yanes-register-render takes a register script (`sample reg value` per line)
// and writes a WAV. We assert the same script produces bit-identical output
// across two invocations. This catches:
//   - non-determinism introduced by uninitialized chip state
//   - build-time vs. config-time random seeding
//   - drift in the ymfm-emulated chip when its internal tables move
//
// This is a different surface than tests/state_roundtrip_tests.cpp (which
// covers CLAP-side state) and the Furnace envelope oracle (which covers
// CLAP-side render). It exercises the low-level chip emulator that the
// register-render binary uses directly.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

constexpr uint32_t kRate = 48000;

struct Write { uint64_t sample; uint32_t reg; uint8_t value; };

// Build a simple register script. We can't know a priori the chip that the
// caller is exercising, so the script just exercises the four registers a
// generic FM chip (or YM2612 in our out-of-the-box setup) responds to. The
// key invariant is *determinism* — the script must produce the same WAV each
// time it is fed to yanes-register-render.
std::string write_script(const std::string& path) {
  std::ofstream f(path);
  // Every chip: key-on (0x28), channel/freq-MSB (0xa4), freq-LSB (0xa0).
  // Note: register addresses and meanings are chip-specific; this is a
  // smoke-level stimulus that produces some audio. The test only checks
  // that two renders of the same script agree.
  f << "0 0x28 0x00  # key off\n"
       "0 0xa4 0x4a  # freq MSB/LSB upper bits\n"
       "0 0xa0 0x69  # freq LSB lower bits\n"
       "1000 0x28 0xf0  # key on (channel bits 0xf0)\n"
       "10000 0x28 0x00  # key off\n";
  return path;
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

void run_case(const char* chip, const char* yanes_register_render) {
  char script_template[] = "/tmp/yrr-script-XXXXXX.reg";
  char wav1[]            = "/tmp/yrr-out1-XXXXXX.wav";
  char wav2[]            = "/tmp/yrr-out2-XXXXXX.wav";
  int fd = mkstemps(script_template, 4);
  if (fd < 0) {
    std::fprintf(stderr, "FAIL: mkstemps(script) failed\n");
    std::abort();
  }
  close(fd);
  fd = mkstemps(wav1, 4);
  if (fd < 0) { std::fprintf(stderr, "FAIL: mkstemps(wav1) failed\n"); std::abort(); }
  close(fd);
  fd = mkstemps(wav2, 4);
  if (fd < 0) { std::fprintf(stderr, "FAIL: mkstemps(wav2) failed\n"); std::abort(); }
  close(fd);

  write_script(script_template);

  const std::string cmd1 = std::string(yanes_register_render) + " " + chip + " " +
                            script_template + " " + wav1 + " >/dev/null 2>&1";
  const std::string cmd2 = std::string(yanes_register_render) + " " + chip + " " +
                            script_template + " " + wav2 + " >/dev/null 2>&1";
  if (std::system(cmd1.c_str()) != 0) {
    std::fprintf(stderr, "FAIL: yanes-register-render returned non-zero on first render of %s\n", chip);
    std::abort();
  }
  if (std::system(cmd2.c_str()) != 0) {
    std::fprintf(stderr, "FAIL: yanes-register-render returned non-zero on second render of %s\n", chip);
    std::abort();
  }

  const std::vector<float> a = load_wav_mono(wav1);
  const std::vector<float> b = load_wav_mono(wav2);
  if (a.size() != b.size()) {
    std::fprintf(stderr, "FAIL: %s render length differs (%zu vs %zu) across two runs\n",
                 chip, a.size(), b.size());
    std::abort();
  }
  const double mad = max_abs_diff(a, b);
  if (mad != 0.0) {
    std::fprintf(stderr, "FAIL: %s render is not bit-identical between two runs (max abs diff %g)\n",
                 chip, mad);
    std::abort();
  }
  std::printf("  %-8s render is bit-identical across two runs\n", chip);

  std::remove(script_template);
  std::remove(wav1);
  std::remove(wav2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <path_to_yanes-register-render>\n", argv[0]);
    return 1;
  }
  std::printf("================================================================\n");
  std::printf("yanes-register-render determinism oracle\n");
  std::printf("================================================================\n");
  // Iterate over the chips yanes-register-render supports. Each is an independent
  // code path inside the emulator; a drift in any one fails this suite.
  const char* chips[] = {"ym2612", "ym2203", "ym2608", "ym2151", "ym3812", "ymf262"};
  for (const char* chip : chips) {
    run_case(chip, argv[1]);
  }
  std::printf("================================================================\n");
  std::printf("Register-render determinism: PASS\n");
  std::printf("================================================================\n");
  return 0;
}
