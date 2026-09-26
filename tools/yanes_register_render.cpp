#include <ymfm_opl.h>
#include <ymfm_opm.h>
#include <ymfm_opn.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct Interface final : ymfm::ymfm_interface {};
struct Write { uint64_t sample{}; uint32_t reg{}; uint8_t value{}; };

template<class Chip> int render(const char* script_path, const char* wav_path, uint32_t clock) {
  std::ifstream text(script_path);
  std::vector<Write> writes;
  std::string line;
  uint64_t duration = 0;
  while (std::getline(text, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    Write w; std::string reg, value;
    if (!(row >> w.sample >> reg >> value)) { std::cerr << "bad script line: " << line << '\n'; return 1; }
    try {
      size_t reg_end = 0, value_end = 0;
      const auto r = std::stoul(reg, &reg_end, 16), v = std::stoul(value, &value_end, 16);
      if (reg_end != reg.size() || value_end != value.size() || r > 0x1ff || v > 0xff ||
          (!writes.empty() && w.sample < writes.back().sample)) {
        std::cerr << "register script contains an invalid value or decreasing timestamp\n"; return 1;
      }
      w.reg = static_cast<uint32_t>(r); w.value = static_cast<uint8_t>(v);
    } catch (const std::exception&) {
      std::cerr << "invalid hexadecimal register/value\n"; return 1;
    }
    writes.push_back(w); duration = std::max(duration, w.sample + 1);
  }
  if (!text.eof() || writes.empty()) { std::cerr << "empty or unreadable register script\n"; return 1; }
  Interface intf; Chip chip(intf); chip.reset();
  const uint32_t rate = chip.sample_rate(clock);
  duration += rate * 2U;
  std::vector<int16_t> pcm; pcm.reserve(static_cast<size_t>(duration) * 2U);
  size_t next = 0;
  for (uint64_t sample = 0; sample < duration; ++sample) {
    while (next < writes.size() && writes[next].sample == sample) {
      const auto& w = writes[next++]; chip.write((w.reg >> 8U) * 2U, static_cast<uint8_t>(w.reg));
      chip.write((w.reg >> 8U) * 2U + 1U, w.value);
    }
    typename Chip::output_data out{}; chip.generate(&out);
    const int32_t left = out.data[0];
    const int32_t right = out.data[Chip::OUTPUTS > 1 ? 1 : 0];
    pcm.push_back(static_cast<int16_t>(std::clamp(left, -32768, 32767)));
    pcm.push_back(static_cast<int16_t>(std::clamp(right, -32768, 32767)));
  }
  std::ofstream wav(wav_path, std::ios::binary);
  auto put16 = [&wav](uint16_t v) { wav.put(static_cast<char>(v)); wav.put(static_cast<char>(v >> 8)); };
  auto put32 = [&put16](uint32_t v) { put16(static_cast<uint16_t>(v)); put16(static_cast<uint16_t>(v >> 16)); };
  wav.write("RIFF", 4); put32(static_cast<uint32_t>(pcm.size() * 2U + 36U)); wav.write("WAVEfmt ", 8);
  put32(16); put16(1); put16(2); put32(rate); put32(rate * 4U); put16(4); put16(16);
  wav.write("data", 4); put32(static_cast<uint32_t>(pcm.size() * 2U));
  wav.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2U));
  std::cout << "rendered " << pcm.size() / 2U << " native-rate frames at " << rate << " Hz\n";
  return wav ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: yanes-register-render CHIP registers.txt output.wav\n"
                 "chips: ym2203 ym2608 ym2612 ym2151 ym3812 ymf262\n"; return 2;
  }
  const std::string chip = argv[1];
  if (chip == "ym2203") return render<ymfm::ym2203>(argv[2], argv[3], 4000000);
  if (chip == "ym2608") return render<ymfm::ym2608>(argv[2], argv[3], 8000000);
  if (chip == "ym2612") return render<ymfm::ym2612>(argv[2], argv[3], 7670454);
  if (chip == "ym2151") return render<ymfm::ym2151>(argv[2], argv[3], 3579545);
  if (chip == "ym3812") return render<ymfm::ym3812>(argv[2], argv[3], 3579545);
  if (chip == "ymf262") return render<ymfm::ymf262>(argv[2], argv[3], 14318180);
  std::cerr << "unknown chip\n"; return 2;
}
