#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint16_t u16(const std::vector<uint8_t>& b, size_t p) {
  return static_cast<uint16_t>(b[p] | (static_cast<uint16_t>(b[p + 1]) << 8));
}
static uint32_t u32(const std::vector<uint8_t>& b, size_t p) {
  return static_cast<uint32_t>(b[p] | (static_cast<uint32_t>(b[p + 1]) << 8) |
      (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24));
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: yanes-dpcm input.wav output.ydmc\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> wav((std::istreambuf_iterator<char>(in)), {});
  if (wav.size() < 44 || std::string(reinterpret_cast<char*>(wav.data()), 4) != "RIFF" ||
      std::string(reinterpret_cast<char*>(wav.data() + 8), 4) != "WAVE") {
    std::cerr << "input is not a RIFF/WAVE file\n"; return 1;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t data_at = 0, data_size = 0;
  for (size_t p = 12; p + 8 <= wav.size();) {
    const uint32_t n = u32(wav, p + 4);
    if (p + 8U + n > wav.size()) break;
    const std::string id(reinterpret_cast<char*>(wav.data() + p), 4);
    if (id == "fmt " && n >= 16) {
      format = u16(wav, p + 8); channels = u16(wav, p + 10);
      rate = u32(wav, p + 12); bits = u16(wav, p + 22);
    } else if (id == "data") { data_at = p + 8; data_size = n; }
    p += 8U + n + (n & 1U);
  }
  if (format != 1 || channels < 1 || channels > 2 || bits != 16 || rate == 0 || data_size < 2U * channels) {
    std::cerr << "requires mono/stereo 16-bit PCM WAV\n"; return 1;
  }
  constexpr double target_rate = 16744.0; // fastest NTSC 2A03 DPCM rate
  const size_t frames = data_size / (2U * channels);
  const size_t out_bits = static_cast<size_t>(frames * target_rate / rate);
  if (!out_bits || out_bits > 8U * 1024U * 1024U) {
    std::cerr << "sample must encode to between one bit and 1 MiB of DPCM\n"; return 1;
  }
  std::vector<uint8_t> encoded((out_bits + 7U) / 8U, 0);
  int level = 64;
  for (size_t i = 0; i < out_bits; ++i) {
    const size_t frame = std::min(frames - 1U, static_cast<size_t>(i * rate / target_rate));
    int sum = 0;
    for (uint16_t ch = 0; ch < channels; ++ch) {
      const size_t q = data_at + (frame * channels + ch) * 2U;
      sum += static_cast<int16_t>(u16(wav, q));
    }
    const int target = std::clamp(64 + (sum / channels) * 60 / 32768, 0, 127);
    const bool up = target >= level;
    level = std::clamp(level + (up ? 2 : -2), 0, 127);
    if (up) encoded[i >> 3U] |= static_cast<uint8_t>(1U << (i & 7U));
  }
  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
  if (!out) { std::cerr << "could not write output\n"; return 1; }
  std::cout << "encoded " << out_bits << " DPCM bits (" << encoded.size() << " bytes)\n";
}
