// Replays 2A03 register writes captured from a real ROM run by patched Nestopia.
//
// The capture side (third_party/nestopia-apu-register-log.patch) records every
// CPU write to $4000-$4017 with a frame index and a frame-local cycle count.
// This tool reconstructs the channel state machines from that stream alone and
// renders through the YANES oscillator primitives, so a passing score means the
// YANES DSP reproduces what the real hardware produced from the same registers
// rather than that two copies of one emulator agree.
#include "../src/dsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kCpuClock = yanes::kCpuClock;
constexpr double kFps = 60.098814;                 // NTSC 2C02 frame rate
constexpr double kCyclesPerFrame = kCpuClock / kFps;
constexpr int kRate = 48000;

// Frame-sequencer step boundaries in CPU cycles (NTSC).
constexpr int kStep4[4] = {7457, 14913, 22371, 29829};
constexpr int kStep5[5] = {7457, 14913, 22371, 29829, 37281};

constexpr std::array<uint8_t, 32> kLengthTable{
    10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

struct Write {
  uint64_t cycle;
  uint16_t reg;
  uint8_t value;
};

struct Envelope {
  bool loop{false}, constant{false}, start{false};
  uint8_t volume{0}, divider{0}, decay{0};

  void clock() {
    if (start) {
      start = false;
      decay = 15;
      divider = volume;
    } else if (divider) {
      --divider;
    } else {
      divider = volume;
      if (decay) --decay;
      else if (loop) decay = 15;
    }
  }
  uint8_t output() const { return constant ? volume : decay; }
};

struct Pulse {
  Envelope envelope;
  uint8_t duty{0}, length{0}, sweep_period{0}, sweep_shift{0}, sweep_divider{0};
  uint8_t sequence{0};
  bool halt{false}, enabled{false};
  bool sweep_enabled{false}, sweep_negate{false}, sweep_reload{false};
  bool ones_complement{false};  // Pulse 1 negates with one's complement.
  uint16_t timer{0};
  int counter{0};

  uint16_t target() const {
    const uint16_t delta = static_cast<uint16_t>(timer >> sweep_shift);
    if (!sweep_negate) return static_cast<uint16_t>(timer + delta);
    const int value = static_cast<int>(timer) - delta - (ones_complement ? 1 : 0);
    return value < 0 ? 0 : static_cast<uint16_t>(value);
  }
  bool muted() const { return timer < 8 || target() > 0x7ff; }

  void clock_timer() {  // APU rate: one call every two CPU cycles.
    if (counter <= 0) {
      counter = timer;
      sequence = (sequence + 1) & 7;
    } else {
      --counter;
    }
  }
  void clock_sweep() {
    if (sweep_divider == 0 && sweep_enabled && sweep_shift && !muted())
      timer = target();
    if (sweep_divider == 0 || sweep_reload) {
      sweep_divider = sweep_period;
      sweep_reload = false;
    } else {
      --sweep_divider;
    }
  }
  void clock_length() {
    if (!halt && length) --length;
  }
  int level() const {
    if (!enabled || !length || muted()) return 0;
    static constexpr std::array<double, 4> kDutyFraction{0.125, 0.25, 0.5, 0.75};
    return yanes::pulse_raw(sequence / 8.0, kDutyFraction[duty]) > 0.0f
               ? envelope.output()
               : 0;
  }
};

struct Triangle {
  uint8_t length{0}, linear{0}, linear_reload{0}, sequence{0};
  bool enabled{false}, control{false}, reload_flag{false};
  uint16_t timer{0};
  int counter{0};

  void clock_timer() {  // CPU rate.
    if (counter <= 0) {
      counter = timer;
      if (length && linear) sequence = (sequence + 1) & 31;
    } else {
      --counter;
    }
  }
  void clock_linear() {
    if (reload_flag) linear = linear_reload;
    else if (linear) --linear;
    if (!control) reload_flag = false;
  }
  void clock_length() {
    if (!control && length) --length;
  }
  int level() const {
    if (!enabled) return 0;
    // The sequencer holds its last step when silenced, so the DAC keeps that
    // level instead of snapping to zero -- that hold is audible as a click when
    // it is modelled wrongly.
    const double bipolar = yanes::nes_triangle(sequence / 32.0);
    return static_cast<int>(std::lround((bipolar + 1.0) * 0.5 * 15.0));
  }
};

struct Noise {
  Envelope envelope;
  uint8_t length{0};
  bool enabled{false}, halt{false}, short_mode{false};
  int period{4}, counter{0};
  uint32_t shift{1};

  void clock_timer() {  // CPU rate: the published period table is in CPU cycles.
    if (counter <= 0) {
      counter = period;
      shift = yanes::lfsr_clock(shift, short_mode ? 6U : 1U, 15U);
    } else {
      --counter;
    }
  }
  void clock_length() {
    if (!halt && length) --length;
  }
  int level() const {
    if (!enabled || !length || (shift & 1U)) return 0;
    return envelope.output();
  }
};

// The delta decoder is YANES's; only the sample bytes come from the capture.
// $4012/$4013 name a PRG address, so the byte the DMC was handed over the bus is
// an input to the replay in exactly the way wave-RAM contents are on Game Boy
// and PC Engine -- the timer, the shift register, and the +-2 clamp are not.
struct Dmc {
  std::vector<uint8_t> fetched;
  size_t next_fetch{0};
  uint8_t level{0}, shift{0}, bits{0};
  bool enabled{false}, silent{true};
  int period{yanes::kDpcmPeriods[0]}, counter{0};

  void clock_timer() {  // CPU rate: the published rate table is in CPU cycles.
    if (counter > 0) {
      --counter;
      return;
    }
    counter = period;
    if (!silent) {
      if (shift & 1U) {
        if (level <= 125) level = static_cast<uint8_t>(level + 2);
      } else if (level >= 2) {
        level = static_cast<uint8_t>(level - 2);
      }
    }
    shift = static_cast<uint8_t>(shift >> 1);
    if (bits) --bits;
    if (bits) return;
    bits = 8;
    if (enabled && next_fetch < fetched.size()) {
      shift = fetched[next_fetch++];
      silent = false;
    } else {
      silent = true;
    }
  }
};

struct Apu {
  Pulse pulse[2];
  Triangle triangle;
  Noise noise;
  Dmc dmc;

  bool five_step{false};
  int frame_cycle{0}, frame_step{0};

  Apu() { pulse[0].ones_complement = true; }

  void quarter_frame() {
    pulse[0].envelope.clock();
    pulse[1].envelope.clock();
    noise.envelope.clock();
    triangle.clock_linear();
  }
  void half_frame() {
    pulse[0].clock_length();
    pulse[1].clock_length();
    noise.clock_length();
    triangle.clock_length();
    pulse[0].clock_sweep();
    pulse[1].clock_sweep();
  }

  void write(uint16_t reg, uint8_t value) {
    switch (reg) {
      case 0x4000:
      case 0x4004: {
        Pulse& p = pulse[(reg >> 2) & 1];
        p.duty = static_cast<uint8_t>(value >> 6);
        p.halt = (value & 0x20) != 0;
        p.envelope.loop = p.halt;
        p.envelope.constant = (value & 0x10) != 0;
        p.envelope.volume = static_cast<uint8_t>(value & 15);
        break;
      }
      case 0x4001:
      case 0x4005: {
        Pulse& p = pulse[(reg >> 2) & 1];
        p.sweep_enabled = (value & 0x80) != 0;
        p.sweep_period = static_cast<uint8_t>((value >> 4) & 7);
        p.sweep_negate = (value & 8) != 0;
        p.sweep_shift = static_cast<uint8_t>(value & 7);
        p.sweep_reload = true;
        break;
      }
      case 0x4002:
      case 0x4006: {
        Pulse& p = pulse[(reg >> 2) & 1];
        p.timer = static_cast<uint16_t>((p.timer & 0x700) | value);
        break;
      }
      case 0x4003:
      case 0x4007: {
        Pulse& p = pulse[(reg >> 2) & 1];
        p.timer = static_cast<uint16_t>((p.timer & 0x0ff) | ((value & 7) << 8));
        if (p.enabled) p.length = kLengthTable[value >> 3];
        p.sequence = 0;
        p.envelope.start = true;
        break;
      }
      case 0x4008:
        triangle.control = (value & 0x80) != 0;
        triangle.linear_reload = static_cast<uint8_t>(value & 0x7f);
        break;
      case 0x400a:
        triangle.timer = static_cast<uint16_t>((triangle.timer & 0x700) | value);
        break;
      case 0x400b:
        triangle.timer =
            static_cast<uint16_t>((triangle.timer & 0x0ff) | ((value & 7) << 8));
        if (triangle.enabled) triangle.length = kLengthTable[value >> 3];
        triangle.reload_flag = true;
        break;
      case 0x400c:
        noise.halt = (value & 0x20) != 0;
        noise.envelope.loop = noise.halt;
        noise.envelope.constant = (value & 0x10) != 0;
        noise.envelope.volume = static_cast<uint8_t>(value & 15);
        break;
      case 0x400e:
        noise.short_mode = (value & 0x80) != 0;
        noise.period = yanes::kNoisePeriods[value & 15];
        break;
      case 0x400f:
        if (noise.enabled) noise.length = kLengthTable[value >> 3];
        noise.envelope.start = true;
        break;
      case 0x4010:
        dmc.period = yanes::kDpcmPeriods[value & 15];
        break;
      case 0x4011:
        dmc.level = static_cast<uint8_t>(value & 0x7f);
        break;
      case 0x4015:
        pulse[0].enabled = (value & 1) != 0;
        pulse[1].enabled = (value & 2) != 0;
        triangle.enabled = (value & 4) != 0;
        noise.enabled = (value & 8) != 0;
        dmc.enabled = (value & 16) != 0;
        if (!pulse[0].enabled) pulse[0].length = 0;
        if (!pulse[1].enabled) pulse[1].length = 0;
        if (!triangle.enabled) triangle.length = 0;
        if (!noise.enabled) noise.length = 0;
        break;
      case 0x4017:
        five_step = (value & 0x80) != 0;
        frame_cycle = 0;
        frame_step = 0;
        // Mode 1 clocks the whole sequence once immediately on the write, which
        // is how a driver that rewrites $4017 every frame keeps its envelopes
        // moving at the video rate.
        if (five_step) {
          quarter_frame();
          half_frame();
        }
        break;
      case 0x4100:
        dmc.fetched.push_back(value);
        break;
      default:
        break;  // $4012-$4014/$4016 carry addresses or controller state.
    }
  }

  void clock_cpu_cycle(uint64_t cycle) {
    triangle.clock_timer();
    noise.clock_timer();
    dmc.clock_timer();
    if ((cycle & 1U) == 0) {
      pulse[0].clock_timer();
      pulse[1].clock_timer();
    }

    ++frame_cycle;
    if (five_step) {
      if (frame_step < 5 && frame_cycle == kStep5[frame_step]) {
        if (frame_step != 3) quarter_frame();
        if (frame_step == 1 || frame_step == 4) half_frame();
        ++frame_step;
        if (frame_step == 5) {
          frame_step = 0;
          frame_cycle = 0;
        }
      }
    } else {
      if (frame_step < 4 && frame_cycle == kStep4[frame_step]) {
        quarter_frame();
        if (frame_step == 1 || frame_step == 3) half_frame();
        ++frame_step;
        if (frame_step == 4) {
          frame_step = 0;
          frame_cycle = 0;
        }
      }
    }
  }

  // Published 2A03 DAC curves. Both terms are non-linear in the channel sum,
  // which is why a channel cannot be scored in isolation against the real mix.
  double mix(unsigned mask) const {
    const int p1 = (mask & 1U) ? pulse[0].level() : 0;
    const int p2 = (mask & 2U) ? pulse[1].level() : 0;
    const int tri = (mask & 4U) ? triangle.level() : 0;
    const int nse = (mask & 8U) ? noise.level() : 0;
    const int sample = (mask & 16U) ? dmc.level : 0;
    const double square = p1 + p2 > 0 ? 95.88 / (8128.0 / (p1 + p2) + 100.0) : 0.0;
    const double tnd = tri / 8227.0 + nse / 12241.0 + sample / 22638.0;
    return square + (tnd > 0.0 ? 159.79 / (1.0 / tnd + 100.0) : 0.0);
  }
};

// Both sides are DC-blocked the same way so the score measures the chip model
// and not the analog stage: a held triangle step or a $4011 DAC level is a large
// constant offset, and the oracle removes it with a ~0.7 Hz single-pole blocker.
// Anything steeper than that would throw away the bass the ROM actually plays.
struct DcBlocker {
  static constexpr double kPole = 1.0 - 3.0 / 32768.0;
  double previous{}, state{};

  double apply(double x) {
    state = x - previous + kPole * state;
    previous = x;
    return state;
  }
};

void wav(const std::string& path, const std::vector<int16_t>& pcm) {
  std::ofstream out(path, std::ios::binary);
  const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2), riff = 36 + bytes;
  const uint32_t fmt_size = 16, rate = kRate, byte_rate = rate * 4;
  const uint16_t format = 1, channels = 2, align = 4, bits = 16;
  auto w = [&](const void* p, size_t n) { out.write(static_cast<const char*>(p), n); };
  w("RIFF", 4); w(&riff, 4); w("WAVEfmt ", 8); w(&fmt_size, 4); w(&format, 2);
  w(&channels, 2); w(&rate, 4); w(&byte_rate, 4); w(&align, 2); w(&bits, 2);
  w("data", 4); w(&bytes, 4); w(pcm.data(), bytes);
}

std::vector<int16_t> render(const std::vector<Write>& writes, uint64_t samples,
                            unsigned mask) {
  Apu apu;
  std::vector<int16_t> out;
  out.reserve(samples * 2);
  const double cycles_per_sample = kCpuClock / kRate;
  // Full-scale sum of both DAC curves; the excerpt is scored on contour and
  // spectrum, but keeping the headroom exact avoids clipping loud passages.
  constexpr double kFullScale = 0.2578 + 0.4854;
  DcBlocker blocker;
  size_t next = 0;
  uint64_t cycle = 0;
  double accumulator = 0.0;
  double history = 0.0;
  for (uint64_t sample = 0; sample < samples; ++sample) {
    const uint64_t end = static_cast<uint64_t>((sample + 1) * cycles_per_sample);
    uint64_t counted = 0;
    for (; cycle < end; ++cycle) {
      while (next < writes.size() && writes[next].cycle <= cycle) {
        apu.write(writes[next].reg, writes[next].value);
        ++next;
      }
      apu.clock_cpu_cycle(cycle);
      accumulator += apu.mix(mask);
      ++counted;
    }
    const double raw = counted ? accumulator / static_cast<double>(counted) : history;
    accumulator = 0.0;
    history = raw;
    const double scaled =
        std::clamp(blocker.apply(raw) / kFullScale * 32767.0, -32768.0, 32767.0);
    const auto pcm = static_cast<int16_t>(scaled);
    out.push_back(pcm);
    out.push_back(pcm);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: yanes-nes-replay nes_apu.log out_dir [seconds]\n";
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "cannot open " << argv[1] << "\n";
    return 1;
  }
  std::vector<Write> writes;
  std::string line;
  uint64_t last = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    uint64_t frame = 0, cycle = 0, frame_cycles = 0;
    std::string reg, value;
    if (!(row >> frame >> cycle >> frame_cycles >> reg >> value)) continue;
    // The core reports a frame-local master-clock count; the frame length in the
    // same units turns it into a fraction, so the replay never has to know the
    // core's clock divider.
    const double fraction = frame_cycles ? static_cast<double>(cycle) /
                                               static_cast<double>(frame_cycles)
                                         : 0.0;
    const double absolute = (static_cast<double>(frame) + fraction) * kCyclesPerFrame;
    const uint64_t at = static_cast<uint64_t>(std::max(0.0, absolute));
    writes.push_back({at, static_cast<uint16_t>(std::stoul(reg, nullptr, 16)),
                      static_cast<uint8_t>(std::stoul(value, nullptr, 16))});
    last = std::max(last, at);
  }
  std::stable_sort(writes.begin(), writes.end(),
                   [](const Write& a, const Write& b) { return a.cycle < b.cycle; });

  const double seconds = argc >= 4 ? std::atof(argv[3]) : 0;
  const uint64_t samples =
      seconds > 0 ? static_cast<uint64_t>(seconds * kRate)
                  : static_cast<uint64_t>(last / kCpuClock * kRate) + kRate;
  const std::string dir = argv[2];
  wav(dir + "/yanes_nes_mix.wav", render(writes, samples, 0x1fU));
  static constexpr const char* kNames[5] = {"pulse1", "pulse2", "triangle",
                                            "noise", "dmc"};
  for (unsigned ch = 0; ch < 5; ++ch)
    wav(dir + "/yanes_nes_" + kNames[ch] + ".wav", render(writes, samples, 1U << ch));
  std::cout << "replayed " << writes.size() << " 2A03 writes over "
            << samples / static_cast<double>(kRate) << "s\n";
}
