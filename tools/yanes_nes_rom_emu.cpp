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

// Independent of YANES DSP: published NTSC 2A03 figures only.
constexpr double kCpuClock = 1789773.0;
constexpr int kSampleRate = 48000;
constexpr std::array<uint16_t, 16> kNoisePeriods{
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

struct NoteEvent {
  double start_sec;
  double end_sec;
  int channel;    // 0 = Pulse 1, 1 = Pulse 2, 2 = Triangle, 3 = Noise
  int key;        // MIDI note number (0-127)
  int velocity;   // 0-127
  uint8_t duty{2}; // 0 = 12.5%, 1 = 25%, 2 = 50%, 3 = 75%
};

// Cycle-stepped 2A03 APU reference. Deliberately does not call into YANES
// oscillators, LFSR helpers, or post filters — this is the hardware gate that
// YANES presets are scored against.
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
    bool short_mode{false};
    bool active{false};
  } noise;

  double dc_in{0.0};
  double dc_out{0.0};
  double mix_acc{0.0};
  double mix_count{0.0};
  double lp_y{0.0};
  double sample_acc{0.0};
  double sample_period{kCpuClock / kSampleRate};
  // One-pole low-pass approximating the console's analog path before RF/AV
  // (~12 kHz). Independent of YANES filters; keeps the hardware gate from
  // scoring infinite-bandwidth aliasing that no real NES outputs.
  double lp_alpha{1.0 - std::exp(-2.0 * 3.14159265358979323846 * 12000.0 / kSampleRate)};
  std::vector<int16_t> pcm_output;

  // Hardware noise period register from the score's MIDI key.
  static int noise_period_index(int key) {
    return std::clamp(key - 48, 0, 15);
  }

  static uint8_t midi_volume(int velocity) {
    // 4-bit APU volume from MIDI velocity (triangle has no volume on hardware).
    return static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(velocity / 127.0 * 15.0)), 0, 15));
  }

  static double midi_hz(int key) {
    return 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
  }

  void note_on(int ch, int key, uint8_t duty_val, int velocity) {
    if (ch == 0 || ch == 1) {
      pulse[ch].active = true;
      pulse[ch].duty = static_cast<uint8_t>(std::min<int>(duty_val, 3));
      pulse[ch].volume = midi_volume(velocity);
      pulse[ch].seq_pos = 0;
      const double hz = midi_hz(key);
      pulse[ch].timer = static_cast<uint16_t>(
          std::clamp(std::round(kCpuClock / (16.0 * hz) - 1.0), 8.0, 2047.0));
      pulse[ch].timer_counter = pulse[ch].timer;
    } else if (ch == 2) {
      // Triangle has no 4-bit volume; length/linear counter gate only.
      triangle.active = true;
      triangle.seq_pos = 0;
      const double hz = midi_hz(key);
      triangle.timer = static_cast<uint16_t>(
          std::clamp(std::round(kCpuClock / (32.0 * hz) - 1.0), 2.0, 2047.0));
      triangle.timer_counter = triangle.timer;
    } else if (ch == 3) {
      noise.active = true;
      noise.volume = midi_volume(velocity);
      const int period_idx = noise_period_index(key);
      noise.period = kNoisePeriods[static_cast<size_t>(period_idx)];
      noise.timer_counter = noise.period;
      noise.short_mode = key >= 64;
      // Hardware does not reseed on note-on; leave the shift register running.
    }
  }

  void note_off(int ch) {
    if (ch == 0 || ch == 1) pulse[ch].active = false;
    else if (ch == 2) triangle.active = false;
    else if (ch == 3) noise.active = false;
  }

  void step_cycle() {
    static constexpr uint8_t duty_table[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0}, // 12.5%
        {0, 1, 1, 0, 0, 0, 0, 0}, // 25%
        {0, 1, 1, 1, 1, 0, 0, 0}, // 50%
        {1, 0, 0, 1, 1, 1, 1, 1}, // 75% / 25% inverted
    };
    static constexpr uint8_t tri_table[32] = {
        15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    for (auto& p : pulse) {
      if (!p.active) continue;
      if (p.timer_counter > 0) {
        --p.timer_counter;
      } else {
        p.timer_counter = p.timer;
        p.seq_pos = static_cast<uint8_t>((p.seq_pos + 1) & 7);
      }
    }

    if (triangle.active) {
      if (triangle.timer_counter > 0) {
        --triangle.timer_counter;
      } else {
        triangle.timer_counter = triangle.timer;
        triangle.seq_pos = static_cast<uint8_t>((triangle.seq_pos + 1) & 31);
      }
    }

    if (noise.active) {
      if (noise.timer_counter > 0) {
        --noise.timer_counter;
      } else {
        noise.timer_counter = noise.period;
        const uint16_t tap = noise.short_mode ? 6U : 1U;
        const uint16_t feedback =
            static_cast<uint16_t>((noise.shift_reg & 1U) ^ ((noise.shift_reg >> tap) & 1U));
        noise.shift_reg =
            static_cast<uint16_t>((noise.shift_reg >> 1U) | (feedback << 14U));
      }
    }

    const double p1 =
        (pulse[0].active && duty_table[pulse[0].duty][pulse[0].seq_pos])
            ? static_cast<double>(pulse[0].volume)
            : 0.0;
    const double p2 =
        (pulse[1].active && duty_table[pulse[1].duty][pulse[1].seq_pos])
            ? static_cast<double>(pulse[1].volume)
            : 0.0;
    const double tr =
        triangle.active ? static_cast<double>(tri_table[triangle.seq_pos]) : 0.0;
    // APU outputs when the LFSR LSB is clear.
    const double ns =
        (noise.active && (noise.shift_reg & 1U) == 0)
            ? static_cast<double>(noise.volume)
            : 0.0;

    // Published non-linear mix (NESdev APU Mixer).
    const double p_sum = p1 + p2;
    const double pulse_out =
        p_sum > 0.0 ? 95.88 / ((8128.0 / p_sum) + 100.0) : 0.0;
    const double tnd_sum = tr / 8227.0 + ns / 12241.0;
    const double tnd_out =
        tnd_sum > 0.0 ? 159.79 / ((1.0 / tnd_sum) + 100.0) : 0.0;
    const double raw = pulse_out + tnd_out;

    // Integrate over the output sample period (not a single CPU-cycle poke) so
    // the reference is a proper decimation of the APU, not a point sample.
    mix_acc += raw;
    mix_count += 1.0;
    sample_acc += 1.0;
    if (sample_acc < sample_period) return;
    sample_acc -= sample_period;

    const double averaged = mix_count > 0.0 ? mix_acc / mix_count : 0.0;
    mix_acc = 0.0;
    mix_count = 0.0;

    lp_y += lp_alpha * (averaged - lp_y);

    // Independent ~90 Hz high-pass (console AC coupling), not YANES's filter.
    constexpr double kDc = 0.995;
    dc_out = kDc * (dc_out + lp_y - dc_in);
    dc_in = lp_y;

    const double total = std::clamp(dc_out * 2.8, -1.0, 1.0);
    pcm_output.push_back(static_cast<int16_t>(total * 32767.0));
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
    std::cerr << "Usage: yanes-nes-rom-emu <--synthetic|rom.nes> <yanes_plugin.clap> <output_dir> "
                 "[duration] [yanes_waveform] [yanes_duty] [yanes_transpose]\n"
                 "  Optional YANES-only overrides are for negative-control gates;\n"
                 "  the independent APU reference always uses the score as written.\n";
    return 1;
  }

  const std::string rom_path = argv[1];
  const std::string clap_path = argv[2];
  const std::string out_dir = argv[3];
  const double duration = (argc >= 5) ? std::atof(argv[4]) : 4.0;
  const double yanes_waveform = (argc >= 6) ? std::atof(argv[5]) : 18.0;
  const double yanes_duty = (argc >= 7) ? std::atof(argv[6]) : -1.0;
  const double yanes_transpose = (argc >= 8) ? std::atof(argv[7]) : 0.0;

  if (!std::isfinite(duration) || duration <= 0 || duration > 3600) {
    std::cerr << "duration must be between zero and 3600 seconds\n"; return 1;
  }
  if (rom_path != "--synthetic") {
    std::ifstream rom_file(rom_path, std::ios::binary);
    char header[16]{};
    if (!rom_file.read(header, sizeof(header)) || std::memcmp(header, "NES\x1a", 4) != 0) {
      std::cerr << "invalid or unreadable iNES header\n"; return 1;
    }
    std::cout << "Legacy filename-selected score; no ROM execution or audio extraction.\n";
  }
  std::cout << "YANES synthetic-score comparison against a separate 2A03 model\n";
  // The default score is generated locally and needs no ROM or external emulator.
  const auto notes = get_rom_music_score(rom_path, duration);
  std::cout << "Prepared " << notes.size() << " note events for the hardware gate.\n";

  // 1. Independent NES APU Hardware Render (reference WAV)
  nes::Apu apu;
  const uint64_t total_cpu_cycles =
      static_cast<uint64_t>(std::llround(duration * nes::kCpuClock));
  const size_t total_samples =
      static_cast<size_t>(std::lround(duration * nes::kSampleRate));

  for (uint64_t cycle = 0; cycle < total_cpu_cycles; ++cycle) {
    for (const auto& n : notes) {
      const uint64_t start_cycle =
          static_cast<uint64_t>(std::llround(n.start_sec * nes::kCpuClock));
      const uint64_t end_cycle =
          static_cast<uint64_t>(std::llround(n.end_sec * nes::kCpuClock));
      if (cycle == start_cycle)
        apu.note_on(n.channel, n.key, n.duty, n.velocity);
      else if (cycle == end_cycle)
        apu.note_off(n.channel);
    }
    apu.step_cycle();
  }

  const std::string rom_wav = out_dir + "/rom_extracted.wav";
  write_wav(rom_wav, apu.pcm_output, 48000, 1);
  std::cout << "Wrote independent APU render to " << rom_wav << " ("
            << apu.pcm_output.size() << " samples).\n";

  // 2. Generate Multi-Track REAPER Project (.rpp)
  const std::string rpp_path = out_dir + "/rom_song.rpp";
  write_reaper_rpp(rpp_path, notes, duration);
  std::cout << "Generated REAPER project from score: " << rpp_path << "\n";

  // 3. Drive YANES CLAP (NES stack preset path) with the same score
  const harness::Library library(clap_path.c_str());
  const clap_plugin_t* plugin = library.create();

  std::vector<int16_t> yanes_pcm;
  yanes_pcm.reserve(total_samples);

  {
    harness::Runner runner(plugin, nes::kSampleRate, 512);
    // Waveform 18 = NES stack by default; optional overrides support negative controls.
    runner.set(0, yanes_waveform);
    if (yanes_duty >= 0.0)
      runner.set(1, yanes_duty);
    else
      runner.set(1, 2.0);  // overridden per pulse note from the score
    if (yanes_transpose != 0.0) runner.set(11, yanes_transpose);
    runner.set(3, 0.0);    // kNoiseMode long; overridden per noise note
    runner.set(4, 0.0);    // kAttackMs
    runner.set(5, 0.0);    // kReleaseMs
    runner.set(9, 0.0);    // kGainDb
    runner.set(14, 0.0);   // kMasterDb
    // Monophonic stack retrigger — same constraint as a real 2A03 channel.
    runner.set(51, 1.0);   // kStrictHardware
    if (yanes_waveform != 18.0 || yanes_duty >= 0.0 || yanes_transpose != 0.0) {
      std::cout << "YANES overrides: waveform=" << yanes_waveform
                << " duty=" << (yanes_duty >= 0.0 ? yanes_duty : -1.0)
                << " transpose=" << yanes_transpose << "\n";
    }

    const size_t total_blocks = total_samples / 512;
    int current_duty = -1;
    int current_noise_period = -1;
    const bool lock_duty = yanes_duty >= 0.0;

    for (size_t b = 0; b < total_blocks; ++b) {
      const double block_start = (b * 512.0) / nes::kSampleRate;
      const double block_end = ((b + 1) * 512.0) / nes::kSampleRate;

      harness::Events events;
      struct Timed {
        uint32_t time;
        int sort_key;
        std::vector<uint8_t> bytes;
      };
      std::vector<Timed> timed;
      auto queue = [&](uint32_t time, int sort_key, const auto& event) {
        Timed item;
        item.time = std::min(time, 511U);
        item.sort_key = sort_key;
        item.bytes.resize(sizeof(event));
        std::memcpy(item.bytes.data(), &event, sizeof(event));
        auto* header = reinterpret_cast<clap_event_header_t*>(item.bytes.data());
        header->time = item.time;
        timed.push_back(std::move(item));
      };

      for (const auto& n : notes) {
        if (n.start_sec >= block_start && n.start_sec < block_end) {
          const uint32_t at = static_cast<uint32_t>(std::lround(
              (n.start_sec - block_start) * nes::kSampleRate));
          if ((n.channel == 0 || n.channel == 1) && !lock_duty &&
              n.duty != current_duty) {
            queue(at, 0, harness::param_event(1, static_cast<double>(n.duty)));
            current_duty = n.duty;
          }
          if (n.channel == 3) {
            // Map hardware period index (key-48) onto YANES's
            // base_period - (key-60) table so both sides hit the same NTSC entry.
            const int period = nes::Apu::noise_period_index(n.key);
            const int base = ((period + (n.key - 60)) % 16 + 16) % 16;
            if (base != current_noise_period) {
              queue(at, 1, harness::param_event(2, static_cast<double>(base)));
              current_noise_period = base;
            }
            queue(at, 2, harness::param_event(3, n.key >= 64 ? 1.0 : 0.0));
          }
          // Triangle has no hardware volume — full gate only.
          const double velocity =
              n.channel == 2 ? 1.0 : std::clamp(n.velocity / 127.0, 0.0, 1.0);
          queue(at, 3,
                harness::note_event(CLAP_EVENT_NOTE_ON,
                                    static_cast<int16_t>(n.channel),
                                    static_cast<int16_t>(n.key), -1, velocity));
        }
        if (n.end_sec >= block_start && n.end_sec < block_end) {
          const uint32_t at = static_cast<uint32_t>(std::lround(
              (n.end_sec - block_start) * nes::kSampleRate));
          queue(at, 4,
                harness::note_event(CLAP_EVENT_NOTE_OFF,
                                    static_cast<int16_t>(n.channel),
                                    static_cast<int16_t>(n.key), -1, 0.0));
        }
      }

      std::stable_sort(timed.begin(), timed.end(),
                       [](const Timed& a, const Timed& b) {
                         if (a.time != b.time) return a.time < b.time;
                         return a.sort_key < b.sort_key;
                       });
      for (const auto& item : timed) {
        if (item.bytes.size() == sizeof(clap_event_param_value_t)) {
          clap_event_param_value_t ev{};
          std::memcpy(&ev, item.bytes.data(), sizeof(ev));
          events.push(ev);
        } else {
          clap_event_note_t ev{};
          std::memcpy(&ev, item.bytes.data(), sizeof(ev));
          events.push(ev);
        }
      }

      runner.run(timed.empty() ? nullptr : &events);

      for (uint32_t i = 0; i < 512; ++i) {
        const float val = std::clamp(runner.left()[i], -1.0f, 1.0f);
        yanes_pcm.push_back(static_cast<int16_t>(val * 32767.0f));
      }
    }
  }

  plugin->destroy(plugin);

  const std::string yanes_wav = out_dir + "/yanes_extracted.wav";
  write_wav(yanes_wav, yanes_pcm, 48000, 1);
  std::cout << "Rendered YANES preset path: " << yanes_wav << " ("
            << yanes_pcm.size() << " samples).\n";

  return 0;
}
