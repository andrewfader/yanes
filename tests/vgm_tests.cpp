#include "platform_test.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[at + static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
}

int run(const std::string& command) {
  return std::system((command + " >" + test_platform::null_device() + " 2>&1").c_str());
}

int main(int argc, char** argv) {
  assert(argc == 2);
  struct Chip { const char* name; uint8_t first, second; uint32_t clock, divider; };
  constexpr std::array chips{
      Chip{"ym2203", 0x55, 0, 4000000, 72}, Chip{"ym2608", 0x56, 0x57, 8000000, 144},
      Chip{"ym2612", 0x52, 0x53, 7670454, 144}, Chip{"ym2151", 0x54, 0, 3579545, 64},
      Chip{"ym3812", 0x5a, 0, 3579545, 72}, Chip{"ymf262", 0x5e, 0x5f, 14318180, 288}};
  const fs::path dir = fs::temp_directory_path() / ("yanes-vgm-tests-" + std::to_string(test_platform::process_id()));
  fs::create_directories(dir);

  for (const Chip& chip : chips) {
    std::vector<uint8_t> vgm(0x40, 0);
    vgm[0] = 'V'; vgm[1] = 'g'; vgm[2] = 'm'; vgm[3] = ' ';
    // An unrelated chip write must be consumed but omitted.
    vgm.insert(vgm.end(), {0x50, 0x90, 0x51, 0x22, 0x11, 0x61, 0x44, 0xac});
    const uint8_t command = chip.second ? chip.second : chip.first;
    vgm.insert(vgm.end(), {command, 0x33, 0x7f, 0x66});
    put32(vgm, 4, static_cast<uint32_t>(vgm.size() - 4));
    const fs::path input = dir / (std::string(chip.name) + ".vgm");
    const fs::path output = dir / (std::string(chip.name) + ".reg");
    std::ofstream file(input, std::ios::binary);
    file.write(reinterpret_cast<const char*>(vgm.data()), static_cast<std::streamsize>(vgm.size()));
    file.close();
    const std::string command_line = "\"" + std::string(argv[1]) + "\" " + chip.name + " \"" +
                                     input.string() + "\" \"" + output.string() + "\"";
    assert(run(command_line) == 0);
    std::ifstream extracted(output);
    std::string line;
    std::getline(extracted, line);
    uint64_t timestamp{}; std::string reg, value;
    extracted >> timestamp >> reg >> value;
    const uint64_t expected = 44100ULL * chip.clock / (static_cast<uint64_t>(chip.divider) * 44100ULL);
    assert(timestamp == expected);
    assert(reg == (chip.second ? "133" : "033"));
    assert(value == "7f");
  }

  assert(run("\"" + std::string(argv[1]) + "\" bad-chip nowhere nowhere") != 0);
  fs::remove_all(dir);
}
