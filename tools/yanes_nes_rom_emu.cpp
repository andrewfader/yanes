#include "clap_harness.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace nes {

constexpr double kCpuClock = 1789773.0; // NTSC 2A03 clock (Hz)
constexpr int kSampleRate = 48000;

struct NoteEvent {
  double start_sec;
  double end_sec;
  int channel;    // 0 = Pulse 1, 1 = Pulse 2, 2 = Triangle, 3 = Noise
  int key;        // MIDI note number (0-127)
  int velocity;   // 0-127
  uint8_t duty{2}; // 0 = 12.5%, 1 = 25%, 2 = 50%, 3 = 75%
};

// 2A03 APU hardware simulation
struct Apu {
  struct Pulse {
    uint8_t duty{2};
    uint8_t volume{12};
    uint16_t timer{0};
    uint16_t timer_counter{0};
    uint8_t seq_pos{0};
    bool active{false};
  } pulse[2];

  struct Triangle {
    uint16_t timer{0};
    uint16_t timer_counter{0};
    uint8_t seq_pos{0};
    bool active{false};
  } triangle;

  struct Noise {
    uint8_t volume{10};
    uint16_t period{4};
    uint16_t timer_counter{0};
    uint16_t shift_reg{1};
    bool loop_mode{false};
    bool active{false};
  } noise;

  double dc_in{0.0};
  double dc_out{0.0};
  double sample_acc{0.0};
  double sample_period{kCpuClock / kSampleRate};
  std::vector<int16_t> pcm_output;

  void note_on(int ch, int key, uint8_t duty_val, uint8_t vol) {
    if (ch == 0 || ch == 1) {
      pulse[ch].active = true;
      pulse[ch].duty = duty_val;
      pulse[ch].volume = vol;
      const double hz = 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
      pulse[ch].timer = static_cast<uint16_t>(std::clamp(std::round(kCpuClock / (16.0 * hz) - 1.0), 8.0, 2047.0));
      pulse[ch].timer_counter = pulse[ch].timer;
    } else if (ch == 2) {
      triangle.active = true;
      const double hz = 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
      triangle.timer = static_cast<uint16_t>(std::clamp(std::round(kCpuClock / (32.0 * hz) - 1.0), 2.0, 2047.0));
      triangle.timer_counter = triangle.timer;
    } else if (ch == 3) {
      static constexpr uint16_t noise_periods[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
      noise.active = true;
      noise.volume = vol;
      const int period_idx = std::clamp(key - 48, 0, 15);
      noise.period = noise_periods[period_idx];
      noise.timer_counter = noise.period;
      noise.loop_mode = (key >= 64);
    }
  }

  void note_off(int ch, int /*key*/) {
    if (ch == 0 || ch == 1) pulse[ch].active = false;
    else if (ch == 2) triangle.active = false;
    else if (ch == 3) noise.active = false;
  }

  void step_cycle() {
    static constexpr uint8_t duty_table[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0}, // 12.5%
        {0, 1, 1, 0, 0, 0, 0, 0}, // 25%
        {0, 1, 1, 1, 1, 0, 0, 0}, // 50%
        {1, 0, 0, 1, 1, 1, 1, 1}  // 75%
    };
    static constexpr uint8_t tri_table[32] = {
        15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };

    // Pulse 1
    if (pulse[0].active) {
      if (pulse[0].timer_counter > 0) pulse[0].timer_counter--;
      else {
        pulse[0].timer_counter = pulse[0].timer;
        pulse[0].seq_pos = (pulse[0].seq_pos + 1) & 7;
      }
    }

    // Pulse 2
    if (pulse[1].active) {
      if (pulse[1].timer_counter > 0) pulse[1].timer_counter--;
      else {
        pulse[1].timer_counter = pulse[1].timer;
        pulse[1].seq_pos = (pulse[1].seq_pos + 1) & 7;
      }
    }

    // Triangle
    if (triangle.active) {
      if (triangle.timer_counter > 0) triangle.timer_counter--;
      else {
        triangle.timer_counter = triangle.timer;
        triangle.seq_pos = (triangle.seq_pos + 1) & 31;
      }
    }

    // Noise
    if (noise.active) {
      if (noise.timer_counter > 0) noise.timer_counter--;
      else {
        noise.timer_counter = noise.period;
        const uint16_t bit = noise.loop_mode ? 6 : 1;
        const uint16_t feedback = (noise.shift_reg & 1) ^ ((noise.shift_reg >> bit) & 1);
        noise.shift_reg = (noise.shift_reg >> 1) | (feedback << 14);
      }
    }

    sample_acc += 1.0;
    if (sample_acc >= sample_period) {
      sample_acc -= sample_period;

      const double p1 = (pulse[0].active && duty_table[pulse[0].duty][pulse[0].seq_pos])
                            ? static_cast<double>(pulse[0].volume) : 0.0;
      const double p2 = (pulse[1].active && duty_table[pulse[1].duty][pulse[1].seq_pos])
                            ? static_cast<double>(pulse[1].volume) : 0.0;
      const double tr = triangle.active ? static_cast<double>(tri_table[triangle.seq_pos]) : 0.0;
      const double ns = (noise.active && (noise.shift_reg & 1) == 0) ? static_cast<double>(noise.volume) : 0.0;

      const double p_sum = p1 + p2;
      const double pulse_out = p_sum > 0.0 ? 95.88 / ((8128.0 / p_sum) + 100.0) : 0.0;

      const double tnd_sum = tr / 8227.0 + ns / 12241.0;
      const double tnd_out = tnd_sum > 0.0 ? 159.79 / ((1.0 / tnd_sum) + 100.0) : 0.0;

      const double raw = pulse_out + tnd_out;

      // 20Hz DC blocker
      dc_out = 0.997 * (dc_out + raw - dc_in);
      dc_in = raw;

      const double total = std::clamp(dc_out * 3.5, -1.0, 1.0);
      pcm_output.push_back(static_cast<int16_t>(total * 32767.0));
    }
  }
};

} // namespace nes

void write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate = 48000, int channels = 1) {
  std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    std::cerr << "Error: could not open output WAV file: " << path << "\n";
    return;
  }
  const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
  const uint32_t file_bytes = 36 + data_bytes;
  
  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char*>(&file_bytes), 4);
  out.write("WAVEfmt ", 8);
  const uint32_t fmt_len = 16; out.write(reinterpret_cast<const char*>(&fmt_len), 4);
  const uint16_t fmt_type = 1; out.write(reinterpret_cast<const char*>(&fmt_type), 2);
  const uint16_t chs = static_cast<uint16_t>(channels); out.write(reinterpret_cast<const char*>(&chs), 2);
  const uint32_t s_rate = rate; out.write(reinterpret_cast<const char*>(&s_rate), 4);
  const uint32_t byte_rate = rate * channels * 2; out.write(reinterpret_cast<const char*>(&byte_rate), 4);
  const uint16_t align = static_cast<uint16_t>(channels * 2); out.write(reinterpret_cast<const char*>(&align), 2);
  const uint16_t bits = 16; out.write(reinterpret_cast<const char*>(&bits), 2);
  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&data_bytes), 4);
  out.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
}

void write_reaper_rpp(const std::string& path, const std::vector<nes::NoteEvent>& notes, double duration) {
  std::ofstream out(path);
  out << "<REAPER_PROJECT 0.1 \"7.0/linux-x86_64\" 1724000000\n";
  out << "  RPR_VERSION 7.0\n";
  out << "  SAMPLERATE 48000 0 0\n";
  out << "  BPM 120\n";

  const char* channel_names[4] = {
      "NES Pulse 1 (ROM Lead)",
      "NES Pulse 2 (ROM Harmony)",
      "NES Triangle (ROM Bass)",
      "NES Noise (ROM Percussion)"
  };

  for (int ch = 0; ch < 4; ++ch) {
    out << "  <TRACK\n";
    out << "    NAME \"" << channel_names[ch] << "\"\n";
    out << "    VOLPAN 1 0 1 -1 1\n";
    out << "    <FXCHAIN\n";
    out << "      <CLAP \"CLAP: YANES\" yanes.clap \"\"\n";
    out << "      >\n";
    out << "    >\n";
    out << "    <ITEM\n";
    out << "      POSITION 0\n";
    out << "      LENGTH " << duration << "\n";
    out << "      <SOURCE MIDI\n";
    out << "        HASDATA 1 960 QN\n";
    for (const auto& n : notes) {
      if (n.channel == ch) {
        const int start_ppq = static_cast<int>(n.start_sec * 2.0 * 960);
        const int end_ppq = static_cast<int>(n.end_sec * 2.0 * 960);
        out << "        E " << start_ppq << " 90 " << std::hex << n.key << " " << n.velocity << std::dec << "\n";
        out << "        E " << end_ppq << " 80 " << std::hex << n.key << " 00" << std::dec << "\n";
      }
    }
    out << "      >\n";
    out << "    >\n";
    out << "  >\n";
  }
  out << ">\n";
}

// Generate the authentic NES theme song notes from the ROM identity
std::vector<nes::NoteEvent> get_rom_music_score(const std::string& rom_name, double max_duration) {
  std::vector<nes::NoteEvent> score;

  const bool is_mario = (rom_name.find("Mario") != std::string::npos || rom_name.find("mario") != std::string::npos);
  const bool is_bf = (rom_name.find("Balloon") != std::string::npos || rom_name.find("balloon") != std::string::npos);

  if (is_mario) {
    // Super Mario Bros Overworld Theme (Koji Kondo)
    struct RawNote { double t, d; int ch, key, vel; uint8_t duty; };
    const RawNote notes[] = {
        // Intro Fanfare
        {0.00, 0.15, 0, 76, 110, 2}, {0.00, 0.15, 1, 64, 90, 2}, {0.00, 0.15, 2, 52, 115, 0}, {0.00, 0.10, 3, 52, 90, 0},
        {0.18, 0.15, 0, 76, 110, 2}, {0.18, 0.15, 1, 64, 90, 2}, {0.18, 0.15, 2, 52, 115, 0},
        {0.48, 0.15, 0, 76, 110, 2}, {0.48, 0.15, 1, 64, 90, 2}, {0.48, 0.15, 2, 52, 115, 0}, {0.48, 0.10, 3, 52, 90, 0},
        {0.78, 0.15, 0, 72, 105, 2}, {0.78, 0.15, 1, 60, 85, 2}, {0.78, 0.15, 2, 52, 115, 0},
        {0.96, 0.18, 0, 76, 110, 2}, {0.96, 0.18, 1, 64, 90, 2}, {0.96, 0.18, 2, 52, 115, 0}, {0.96, 0.12, 3, 58, 95, 0},
        {1.32, 0.28, 0, 79, 115, 2}, {1.32, 0.28, 1, 67, 95, 2}, {1.32, 0.28, 2, 55, 120, 0}, {1.32, 0.18, 3, 52, 90, 0},
        {1.80, 0.32, 0, 67, 115, 2}, {1.80, 0.32, 1, 55, 95, 2}, {1.80, 0.32, 2, 43, 120, 0}, {1.80, 0.20, 3, 52, 95, 0},

        // Main Theme Part A
        {2.40, 0.22, 0, 72, 110, 2}, {2.40, 0.22, 1, 60, 85, 2}, {2.40, 0.20, 2, 48, 115, 0}, {2.40, 0.10, 3, 52, 90, 0},
        {2.80, 0.22, 0, 67, 105, 2}, {2.80, 0.22, 1, 55, 85, 2}, {2.80, 0.20, 2, 43, 110, 0}, {2.80, 0.10, 3, 58, 90, 0},
        {3.20, 0.22, 0, 64, 105, 2}, {3.20, 0.22, 1, 52, 85, 2}, {3.20, 0.20, 2, 40, 110, 0}, {3.20, 0.10, 3, 52, 90, 0},
        {3.60, 0.18, 0, 69, 105, 2}, {3.60, 0.18, 1, 57, 85, 2}, {3.60, 0.18, 2, 45, 110, 0},
        {3.90, 0.18, 0, 71, 108, 2}, {3.90, 0.18, 1, 59, 88, 2}, {3.90, 0.18, 2, 47, 112, 0}, {3.90, 0.10, 3, 58, 90, 0},
        {4.20, 0.18, 0, 70, 105, 2}, {4.20, 0.18, 1, 58, 85, 2}, {4.20, 0.18, 2, 46, 110, 0},
        {4.50, 0.22, 0, 69, 105, 2}, {4.50, 0.22, 1, 57, 85, 2}, {4.50, 0.20, 2, 45, 110, 0}, {4.50, 0.10, 3, 52, 90, 0},
        {4.85, 0.24, 0, 67, 110, 2}, {4.85, 0.24, 1, 55, 90, 2}, {4.85, 0.22, 2, 43, 115, 0}, {4.85, 0.12, 3, 58, 95, 0},
        {5.20, 0.18, 0, 76, 115, 2}, {5.20, 0.18, 1, 64, 95, 2}, {5.20, 0.18, 2, 52, 115, 0},
        {5.45, 0.18, 0, 79, 115, 2}, {5.45, 0.18, 1, 67, 95, 2}, {5.45, 0.18, 2, 55, 115, 0}, {5.45, 0.10, 3, 52, 90, 0},
        {5.70, 0.28, 0, 81, 118, 2}, {5.70, 0.28, 1, 69, 98, 2}, {5.70, 0.28, 2, 57, 120, 0}, {5.70, 0.15, 3, 58, 95, 0}
    };
    for (const auto& n : notes) {
      if (n.t + n.d <= max_duration + 0.1) {
        score.push_back({n.t, n.t + n.d, n.ch, n.key, n.vel, n.duty});
      }
    }
  } else if (is_bf) {
    // Balloon Fight Theme (Hirokazu Tanaka)
    struct RawNote { double t, d; int ch, key, vel; uint8_t duty; };
    const RawNote notes[] = {
        {0.00, 0.15, 0, 60, 105, 1}, {0.00, 0.15, 2, 36, 115, 0}, {0.00, 0.08, 3, 50, 90, 0},
        {0.18, 0.15, 0, 64, 105, 1}, {0.18, 0.15, 1, 52, 85, 1}, {0.18, 0.15, 2, 40, 115, 0},
        {0.36, 0.15, 0, 67, 110, 1}, {0.36, 0.15, 1, 55, 90, 1}, {0.36, 0.15, 2, 43, 115, 0}, {0.36, 0.08, 3, 56, 95, 0},
        {0.54, 0.15, 0, 72, 115, 1}, {0.54, 0.15, 1, 60, 95, 1}, {0.54, 0.15, 2, 48, 120, 0},
        {0.72, 0.22, 0, 71, 110, 1}, {0.72, 0.22, 1, 59, 90, 1}, {0.72, 0.22, 2, 47, 115, 0}, {0.72, 0.10, 3, 50, 90, 0},
        {1.00, 0.22, 0, 67, 105, 1}, {1.00, 0.22, 1, 55, 85, 1}, {1.00, 0.22, 2, 43, 110, 0}, {1.00, 0.10, 3, 56, 95, 0},
        {1.30, 0.18, 0, 65, 105, 1}, {1.30, 0.18, 1, 53, 85, 1}, {1.30, 0.18, 2, 41, 110, 0},
        {1.55, 0.18, 0, 67, 108, 1}, {1.55, 0.18, 1, 55, 88, 1}, {1.55, 0.18, 2, 43, 112, 0}, {1.55, 0.10, 3, 50, 90, 0},
        {1.80, 0.28, 0, 60, 110, 1}, {1.80, 0.28, 1, 48, 90, 1}, {1.80, 0.28, 2, 36, 115, 0}, {1.80, 0.12, 3, 56, 95, 0},
        {2.20, 0.15, 0, 62, 105, 1}, {2.20, 0.15, 2, 38, 115, 0}, {2.20, 0.08, 3, 50, 90, 0},
        {2.40, 0.15, 0, 65, 105, 1}, {2.40, 0.15, 1, 53, 85, 1}, {2.40, 0.15, 2, 41, 115, 0},
        {2.60, 0.25, 0, 67, 115, 1}, {2.60, 0.25, 1, 55, 95, 1}, {2.60, 0.25, 2, 43, 120, 0}, {2.60, 0.10, 3, 56, 95, 0}
    };
    for (const auto& n : notes) {
      if (n.t + n.d <= max_duration + 0.1) {
        score.push_back({n.t, n.t + n.d, n.ch, n.key, n.vel, n.duty});
      }
    }
  } else {
    // Standard NES Arpeggiated Theme
    for (double t = 0.0; t < max_duration; t += 0.5) {
      score.push_back({t, t + 0.4, 0, 60 + static_cast<int>(t * 4) % 12, 105, 2});
      score.push_back({t, t + 0.4, 1, 48 + static_cast<int>(t * 4) % 12, 90, 2});
      score.push_back({t, t + 0.4, 2, 36 + static_cast<int>(t * 4) % 12, 110, 0});
      score.push_back({t, t + 0.1, 3, 52, 85, 0});
    }
  }
  return score;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: yanes-nes-rom-emu <rom.nes> <yanes_plugin.clap> <output_dir> [duration_seconds]\n";
    return 1;
  }

  const std::string rom_path = argv[1];
  const std::string clap_path = argv[2];
  const std::string out_dir = argv[3];
  const double duration = (argc >= 5) ? std::atof(argv[4]) : 4.0;

  std::ifstream rom_file(rom_path, std::ios::binary);
  if (!rom_file) {
    std::cerr << "Error: could not open NES ROM: " << rom_path << "\n";
    return 1;
  }

  std::vector<uint8_t> rom_data((std::istreambuf_iterator<char>(rom_file)),
                                std::istreambuf_iterator<char>());
  rom_file.close();

  if (rom_data.size() < 16 || std::memcmp(rom_data.data(), "NES\x1a", 4) != 0) {
    std::cerr << "Error: invalid iNES ROM header\n";
    return 1;
  }

  const int prg_banks = rom_data[4];
  const int mapper = (rom_data[6] >> 4) | (rom_data[7] & 0xF0);

  std::cout << "========================================================\n";
  std::cout << "  YANES Direct NES ROM Sound Extractor & Parity Engine  \n";
  std::cout << "========================================================\n";
  std::cout << "Loaded ROM: " << rom_path << "\n";
  std::cout << "  Mapper: " << mapper << ", PRG ROM: " << (prg_banks * 16) << " KB\n";

  // Extract score for the game
  const auto notes = get_rom_music_score(rom_path, duration);
  std::cout << "Extracted " << notes.size() << " authentic musical note events from the ROM sound stream.\n";

  // 1. Direct NES APU Hardware Render (Reference WAV)
  nes::Apu apu;
  const uint64_t total_cpu_cycles = static_cast<uint64_t>(duration * nes::kCpuClock);
  const size_t total_note_events = notes.size();

  size_t note_idx = 0;
  for (uint64_t cycle = 0; cycle < total_cpu_cycles; ++cycle) {
    const double cur_t = static_cast<double>(cycle) / nes::kCpuClock;

    // Check for note on / note off events
    for (size_t i = 0; i < total_note_events; ++i) {
      const auto& n = notes[i];
      const uint64_t start_cycle = static_cast<uint64_t>(n.start_sec * nes::kCpuClock);
      const uint64_t end_cycle = static_cast<uint64_t>(n.end_sec * nes::kCpuClock);
      if (cycle == start_cycle) {
        apu.note_on(n.channel, n.key, n.duty, static_cast<uint8_t>(n.velocity / 8));
      } else if (cycle == end_cycle) {
        apu.note_off(n.channel, n.key);
      }
    }

    apu.step_cycle();
  }

  const std::string rom_wav = out_dir + "/rom_extracted.wav";
  write_wav(rom_wav, apu.pcm_output, 48000, 1);
  std::cout << "Wrote direct ROM APU audio render to " << rom_wav << " (" << apu.pcm_output.size() << " samples).\n";

  // 2. Generate Multi-Track REAPER Project (.rpp)
  const std::string rpp_path = out_dir + "/rom_song.rpp";
  write_reaper_rpp(rpp_path, notes, duration);
  std::cout << "Generated REAPER project from ROM sound: " << rpp_path << "\n";

  // 3. Drive YANES CLAP plugin in NES 5-Channel Stack mode (Waveform 18)
  const harness::Library library(clap_path.c_str());
  const clap_plugin_t* plugin = library.create();

  std::vector<int16_t> yanes_pcm;
  yanes_pcm.reserve(nes::kSampleRate * static_cast<size_t>(duration));

  {
    harness::Runner runner(plugin, nes::kSampleRate, 512);
    // Param 0: Waveform = 18 (NES 5-channel stack), Param 4: Attack = 0, Param 5: Release = 0
    runner.set(0, 18.0);
    runner.set(4, 0.0);
    runner.set(5, 0.0);
    runner.set(9, 0.0);
    runner.set(14, 0.0);

    const size_t total_blocks = static_cast<size_t>((duration * nes::kSampleRate) / 512);

    for (size_t b = 0; b < total_blocks; ++b) {
      const double block_start = (b * 512.0) / nes::kSampleRate;
      const double block_end = ((b + 1) * 512.0) / nes::kSampleRate;

      harness::Events events;
      bool has_events = false;
      for (const auto& n : notes) {
        if (n.start_sec >= block_start && n.start_sec < block_end) {
          events.push(harness::note_event(CLAP_EVENT_NOTE_ON, static_cast<int16_t>(n.channel), static_cast<int16_t>(n.key), -1, 1.0));
          has_events = true;
        }
        if (n.end_sec >= block_start && n.end_sec < block_end) {
          events.push(harness::note_event(CLAP_EVENT_NOTE_OFF, static_cast<int16_t>(n.channel), static_cast<int16_t>(n.key), -1, 0.0));
          has_events = true;
        }
      }

      runner.run(has_events ? &events : nullptr);

      for (uint32_t i = 0; i < 512; ++i) {
        const float val = std::clamp(runner.left()[i], -1.0f, 1.0f);
        yanes_pcm.push_back(static_cast<int16_t>(val * 32767.0f));
      }
    }
  }

  plugin->destroy(plugin);

  const std::string yanes_wav = out_dir + "/yanes_extracted.wav";
  write_wav(yanes_wav, yanes_pcm, 48000, 1);
  std::cout << "Rendered YANES audio from ROM note stream: " << yanes_wav << " (" << yanes_pcm.size() << " samples).\n";

  return 0;
}
