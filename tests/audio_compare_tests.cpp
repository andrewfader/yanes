#include "platform_test.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
void wav(const fs::path& path, bool silent, bool nonfinite = false) {
  std::ofstream out(path, std::ios::binary);
  auto u16 = [&](unsigned x) { out.put(static_cast<char>(x)); out.put(static_cast<char>(x >> 8)); };
  auto u32 = [&](unsigned x) { u16(x); u16(x >> 16); };
  const unsigned bytes = 4096 * (nonfinite ? 4 : 2);
  out.write("RIFF", 4); u32(bytes + 36); out.write("WAVEfmt ", 8);
  u32(16); u16(nonfinite ? 3 : 1); u16(1); u32(48000);
  u32(48000 * (nonfinite ? 4 : 2)); u16(nonfinite ? 4 : 2); u16(nonfinite ? 32 : 16);
  out.write("data", 4); u32(bytes);
  for (unsigned i = 0; i < 4096; ++i) {
    if (nonfinite) u32(0x7fc00000); // IEEE float NaN
    else u16(silent ? 0 : (i % 32 < 16 ? 12000 : static_cast<unsigned>(-12000)));
  }
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const auto dir = fs::temp_directory_path() / ("yanes-audio-tests-" + std::to_string(test_platform::process_id()));
  fs::create_directories(dir);
  const auto tone = dir / "tone.wav", silence = dir / "silence.wav", nan = dir / "nan.wav";
  wav(tone, false); wav(silence, true); wav(nan, false, true);
  auto run = [&](const fs::path& file, const std::string& threshold) {
    const std::string cmd = "\"" + std::string(argv[1]) + "\" \"" + file.string() + "\" \"" + file.string() + "\" " + threshold + " >" + test_platform::null_device() + " 2>&1";
    return std::system(test_platform::shell_command(cmd).c_str());
  };
  assert(run(tone, "0.999") == 0); // constant energy is a valid self-match
  assert(run(silence, "") != 0);
  assert(run(nan, "") != 0);
  assert(run(tone, "nan") != 0);
  assert(run(tone, "garbage") != 0);
  fs::remove_all(dir);
}
