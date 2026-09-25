// Replays HuC6280 PSG writes captured from a real ROM run by patched Beetle PCE.
//
// The chip is a bank of six 5-bit DACs fed from 32-entry wave RAM, not a bank of
// oscillators: a channel's output is whatever byte its waveform index last
// latched, held until a counter running at the master clock advances the index.
// Modelling it as a continuous phase into an interpolated wavetable gets the
// pitch right and the spectrum wrong, and it cannot reproduce the direct-DAC
// streaming several games use for drums and speech, where the CPU writes the
// output latch faster than the sequencer would ever step it.
//
// So this replays the register stream against a latch-and-counter model, driving
// it from the log's monotonic PSG clock, and integrates the resulting piecewise
// constant waveform exactly over each output sample. Every write lands on the
// master clock it landed on in the emulator, with no quantisation to the host
// sample rate.
//
// Provenance, because it bears on what a pass here proves: the wave-RAM lookup
// and the noise register are the plugin's own primitives from src/dsp.hpp, and
// the gate does test those. The control layer around them -- counters, the
// ultrasonic case, the volume update walk, LFO -- was written by reading
// Mednafen's HuC6280 PSG, which is also the oracle. That half of the lane is a
// regression gate on the capture and the timeline, not independent evidence.
#include "../src/dsp.hpp"
#include "replay_support.hpp"

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
constexpr double kPsgClock = 7159090.909090909;  // PCE master clock / 3.
constexpr int kRate = 44100;
// Integrate at 4x the host rate so the decimation filter, not the sample
// window, sets the top-octave response.
constexpr int kOversample = 4;
constexpr int kSubRate = kRate * kOversample;
constexpr int kChannels = 6;
// A channel whose real playback frequency is this high or higher stops behaving
// as a wavetable and exposes the average of its wave RAM instead.
constexpr int kUltrasonic = 0x7;

struct Write {
  int64_t clock;
  uint8_t reg, value;
};

enum class Output { Off, Norm, Noise, Accum };

// The DAC ladder is unipolar on the original HuC6280 and centred on the
// HuC6280A in the SuperGrafx; a plain PC Engine ROM runs on the former, whose
// per-channel DC offset is what makes volume automation audible as a step.
bool g_centred_dac = false;

struct Dac {
  double table[32][32]{};   // [attenuation][wave sample]
  double volume_only[32]{};
  Dac() {
    for (int level = 0; level < 32; ++level) {
      // Each attenuation step is a ~1.5 dB reduction; 31 is silence.
      double gain = level == 0x1f ? 0.0 : (8.0 / 6.0) / std::exp2(level / 4.0);
      volume_only[level] = gain * 65536.0;
      for (int sample = 0; sample < 32; ++sample) {
        const int effective =
            g_centred_dac ? sample * 2 - 0x1f : sample * 2;
        table[level][sample] = gain * effective * 128.0;
      }
    }
  }
};

struct Channel {
  uint16_t frequency{0};
  uint8_t control{0}, balance{0}, noise_control{0};
  uint8_t waveform_index{0}, dda{0};
  std::array<uint8_t, 32> waveform{};
  int32_t wave_sum{0};
  uint32_t lfsr{1};
  int64_t counter{0}, noise_count{1};
  int32_t frequency_clocks{0}, noise_clocks{0};
  int32_t attenuation[2]{0x1f, 0x1f};
  Output output{Output::Off};

  // Exact integration of the piecewise-constant DAC output.
  yanes::replay::Integrator dac{};
};

// The cell the waveform index currently addresses, read through the plugin's
// own wave-RAM primitive and converted back to the five-bit code the DAC ladder
// is indexed by.
int wave_cell(const Channel& ch) {
  const double phase = (ch.waveform_index + 0.5) / 32.0;
  const float sample = yanes::pce_wave_sample(phase, ch.waveform.data());
  return std::clamp(static_cast<int>(std::lround((sample + 1.0f) * 15.5f)), 0, 31);
}

struct Psg {
  std::array<Channel, kChannels> channel{};
  uint8_t select{0}, global_balance{0}, lfo_frequency{0}, lfo_control{0};
  int64_t volume_counter{0}, volume_which{0}, volume_latch{0};
  bool volume_pending{false};
  int64_t last_timestamp{0}, window_start{0};
  Dac ladder{};

  double sample_level(const Channel& ch, int side) const {
    switch (ch.output) {
      case Output::Off: return 0.0;
      case Output::Norm: return ladder.table[ch.attenuation[side]][ch.dda];
      case Output::Noise:
        return ladder.table[ch.attenuation[side]][(ch.lfsr & 1U) ? 0x1f : 0];
      case Output::Accum: {
        // 32 wave cells of 5 bits each; the centred DAC subtracts the midpoint.
        const double centre = g_centred_dac ? 496.0 : 0.0;
        return ladder.volume_only[ch.attenuation[side]] * (ch.wave_sum - centre) /
               8192.0;
      }
    }
    return 0.0;
  }

  // Closes off the span the channel held its previous level for, then latches
  // the level it holds from this clock on.
  void emit(Channel& ch, int64_t clock) {
    ch.dac.hold_until(clock);
    ch.dac.level[0] = sample_level(ch, 0);
    ch.dac.level[1] = sample_level(ch, 1);
  }

  void recalc_output(int index) {
    Channel& ch = channel[index];
    const uint8_t off_mask = g_centred_dac ? 0xc0 : 0x80;
    if (!(ch.control & off_mask))
      ch.output = Output::Off;
    else if (ch.noise_control & ch.control & 0x80)
      ch.output = Output::Noise;
    else if ((ch.control & 0xc0) == 0x80 &&
             ch.frequency_clocks <= kUltrasonic &&
             (index != 1 || !(lfo_control & 0x80)))
      ch.output = Output::Accum;
    else
      ch.output = Output::Norm;
  }

  void recalc_frequency(int index) {
    Channel& ch = channel[index];
    if (index == 0 && (lfo_control & 0x03)) {
      const unsigned shift = ((lfo_control & 0x03) - 1U) << 1U;
      const uint32_t modulated =
          (ch.frequency + (static_cast<uint32_t>(channel[1].dda - 0x10) << shift)) &
          0xfff;
      ch.frequency_clocks = static_cast<int32_t>(modulated ? modulated : 4096) << 1;
      return;
    }
    ch.frequency_clocks = static_cast<int32_t>(ch.frequency ? ch.frequency : 4096) << 1;
    if (index == 1 && (lfo_control & 0x03))
      ch.frequency_clocks *= lfo_frequency ? lfo_frequency : 256;
  }

  void recalc_noise(int index) {
    Channel& ch = channel[index];
    int32_t divider = 0x1f - (ch.noise_control & 0x1f);
    // The zero divider is a hardware special case: 32 clocks, not 31*64.
    if (!divider) divider = 0x20; else divider <<= 6;
    ch.noise_clocks = divider << 1;
  }

  int32_t volume_of(int index, int side) const {
    static constexpr int scale[16] = {0x00, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d,
                                      0x0f, 0x10, 0x13, 0x15, 0x17, 0x19, 0x1b,
                                      0x1d, 0x1f};
    const Channel& ch = channel[index];
    const int shift = side ? 0 : 4;
    const int reduction = (0x1f - scale[(global_balance >> shift) & 0xf]) +
                          (0x1f - scale[(ch.balance >> shift) & 0xf]) +
                          (0x1f - (ch.control & 0x1f));
    return std::min(reduction, 0x1f);
  }

  void power() {
    for (int index = 0; index < kChannels; ++index) {
      Channel& ch = channel[index];
      ch.attenuation[0] = ch.attenuation[1] = 0x1f;
      recalc_frequency(index);
      recalc_output(index);
      ch.counter = ch.frequency_clocks;
      if (index >= 4) recalc_noise(index);
      ch.noise_count = 1;
      ch.lfsr = 1;
    }
  }

  void run_channel(int index, int64_t timestamp, bool lfo_on) {
    Channel& ch = channel[index];
    const int64_t started = ch.dac.last_change;
    if (timestamp <= started) return;
    const int64_t run_time = timestamp - started;
    emit(ch, started);

    if (index >= 4) {
      ch.noise_count -= run_time;
      while (ch.noise_count <= 0) {
        ch.lfsr = yanes::pce_lfsr_clock(ch.lfsr);
        if (ch.output == Output::Noise) emit(ch, timestamp + ch.noise_count);
        ch.noise_count += ch.noise_clocks;
      }
    }

    // A disabled channel, the halted LFO modulator, and direct-DAC mode all
    // stop the waveform sequencer; noise does not, it only overrides the output.
    if (!(ch.control & 0x80) || (index == 1 && (lfo_control & 0x80)) ||
        (ch.control & 0x40)) {
      emit(ch, timestamp);
      return;
    }

    ch.counter -= run_time;
    if (!lfo_on && ch.frequency_clocks <= kUltrasonic) {
      if (ch.counter <= 0) {
        const int64_t steps = ((0 - ch.counter) / ch.frequency_clocks) + 1;
        ch.counter += steps * ch.frequency_clocks;
        ch.waveform_index = static_cast<uint8_t>((ch.waveform_index + steps) & 0x1f);
        ch.dda = wave_cell(ch);
      }
    }

    while (ch.counter <= 0) {
      ch.waveform_index = (ch.waveform_index + 1) & 0x1f;
      ch.dda = wave_cell(ch);
      emit(ch, timestamp + ch.counter);
      if (lfo_on) {
        run_channel(1, timestamp + ch.counter, false);
        recalc_frequency(0);
        recalc_output(0);
        ch.counter += ch.frequency_clocks <= kUltrasonic ? kUltrasonic
                                                         : ch.frequency_clocks;
      } else {
        ch.counter += ch.frequency_clocks;
      }
    }
    emit(ch, timestamp);
  }

  void update(int64_t timestamp) {
    int64_t clocks = timestamp - last_timestamp;
    if (clocks <= 0) return;
    if (volume_pending && !volume_counter && !volume_which) {
      volume_counter = 1;
      volume_pending = false;
    }
    bool lfo_on = (lfo_control & 0x03) != 0;
    if (lfo_on && (!(channel[1].control & 0x80) || (lfo_control & 0x80))) {
      lfo_on = false;
      recalc_frequency(0);
      recalc_output(0);
    }
    int64_t running = last_timestamp;
    while (clocks > 0) {
      int64_t chunk = clocks;
      if (volume_counter > 0 && chunk > volume_counter) chunk = volume_counter;
      running += chunk;
      clocks -= chunk;
      for (int index = 0; index < kChannels; ++index)
        run_channel(index, running, lfo_on && index == 0);
      // Volume changes are not applied at once: the chip walks the twelve
      // channel/side pairs, one every 256 clocks, so a game that ramps volume
      // hears the ramp staggered.
      if (volume_counter > 0) {
        volume_counter -= chunk;
        if (!volume_counter) {
          const int phase = volume_which & 1;
          const int side = static_cast<int>(((volume_which >> 1) & 1) ^ 1);
          const int index = static_cast<int>(volume_which >> 2);
          if (index < kChannels) {
            if (!phase) volume_latch = volume_of(index, side);
            else channel[index].attenuation[side] = static_cast<int32_t>(volume_latch);
          }
          volume_which = (volume_which + 1) & 0x1f;
          if (volume_which) {
            volume_counter = phase ? 1 : 255;
          } else if (volume_pending) {
            volume_counter = phase ? 1 : 255;
            volume_pending = false;
          }
        }
      }
      last_timestamp = running;
    }
  }

  void write(int64_t timestamp, uint8_t reg, uint8_t value) {
    reg &= 0x0f;
    if (reg == 0x00) {
      select = value & 0x07;
      return;
    }
    update(timestamp);
    if (reg == 0x08) {
      lfo_frequency = value;
      return;
    }
    if (reg == 0x09) {
      if (value & 0x80) {
        Channel& lfo = channel[1];
        lfo.waveform_index = 0;
        lfo.dda = wave_cell(lfo);
        lfo.counter = lfo.frequency_clocks;
      }
      lfo_control = value;
      recalc_frequency(0);
      recalc_output(0);
      recalc_frequency(1);
      recalc_output(1);
      return;
    }
    if (reg == 0x01) {
      global_balance = value;
      volume_pending = true;
      return;
    }
    if (select >= kChannels) return;  // No more than six channels, silly game.
    Channel& ch = channel[select];
    switch (reg) {
      case 0x02:
        ch.frequency = (ch.frequency & 0x0f00) | value;
        recalc_frequency(select);
        recalc_output(select);
        break;
      case 0x03:
        ch.frequency = (ch.frequency & 0x00ff) |
                       (static_cast<uint16_t>(value & 0x0f) << 8);
        recalc_frequency(select);
        recalc_output(select);
        break;
      case 0x04:
        if ((ch.control & 0x40) && !(value & 0x40)) {
          ch.waveform_index = 0;
          ch.dda = wave_cell(ch);
          ch.counter = ch.frequency_clocks;
        }
        if (!(ch.control & 0x80) && (value & 0x80) && !(value & 0x40)) {
          ch.waveform_index = (ch.waveform_index + 1) & 0x1f;
          ch.dda = wave_cell(ch);
        }
        ch.control = value;
        recalc_frequency(select);
        recalc_output(select);
        volume_pending = true;
        break;
      case 0x05:
        ch.balance = value;
        volume_pending = true;
        break;
      case 0x06:
        value &= 0x1f;
        if (!(ch.control & 0x40)) {
          ch.wave_sum -= ch.waveform[ch.waveform_index];
          ch.waveform[ch.waveform_index] = value;
          ch.wave_sum += value;
        }
        if ((ch.control & 0xc0) == 0x00)
          ch.waveform_index = (ch.waveform_index + 1) & 0x1f;
        // Writing wave data on an enabled channel latches the DAC directly,
        // whether or not direct-DAC mode is on. This is the write games stream
        // samples through.
        if (ch.control & 0x80) ch.dda = value;
        break;
      case 0x07:
        if (select >= 4) {
          ch.noise_control = value;
          recalc_noise(select);
          recalc_output(select);
        }
        break;
      default:
        break;
    }
  }

  // Returns the mean level each channel held over [from, to), and rearms the
  // integrators for the next window.
  void drain(int64_t to, double (&out)[kChannels][2]) {
    const double span = static_cast<double>(to - window_start);
    window_start = to;
    update(to);
    for (int index = 0; index < kChannels; ++index) {
      Channel& ch = channel[index];
      emit(ch, to);
      for (int side = 0; side < 2; ++side)
        out[index][side] = ch.dac.mean(side, span);
    }
  }
};

// Beetle integrates the chip's output at a quarter of the master clock and runs
// it through a one-pole pair before resampling: a gentle low pass, and a high
// pass that removes the unipolar DAC's standing offset while leaving the step a
// volume change produces.
struct OnePolePair {
  yanes::replay::OnePole low, high;
  OnePolePair() {
    constexpr double kHrRate = kPsgClock / 4.0;
    low.cutoff(kHrRate / 4.0 / (2.0 * yanes::replay::kPi), kSubRate);
    high.cutoff(kHrRate / 16384.0 / (2.0 * yanes::replay::kPi), kSubRate);
  }
  double process(double x) { return high.high(low.low(x)); }
};

using Decimator = yanes::replay::Decimator<kOversample>;

// One pass produces the mix and every isolated channel, so a soloed lane is
// exactly the channel that contributed to the mix rather than a rerun.
struct Render {
  std::vector<int16_t> mix;
  std::array<std::vector<int16_t>, kChannels> solo;
};

Render render(const std::vector<Write>& writes, size_t frames) {
  Psg psg;
  psg.power();
  Render out;
  out.mix.reserve(frames * 2);
  for (auto& channel : out.solo) channel.reserve(frames * 2);

  std::array<std::array<OnePolePair, 2>, kChannels> filter{};
  std::array<std::array<Decimator, 2>, kChannels> decimate{};
  std::array<OnePolePair, 2> mix_filter{};
  std::array<Decimator, 2> mix_decimate{};

  size_t next = 0;
  double window[kChannels][2];
  // Full scale is six channels of a 5-bit unipolar DAC at no attenuation.
  constexpr double scale = 32767.0 / (kChannels * (8.0 / 6.0) * 62.0 * 128.0);
  for (size_t sample = 0; sample < frames; ++sample) {
    double mixed[2][kOversample];
    double soloed[kChannels][2][kOversample];
    for (int sub = 0; sub < kOversample; ++sub) {
      const int64_t clock = static_cast<int64_t>(
          std::llround((sample * kOversample + sub + 1.0) * kPsgClock / kSubRate));
      while (next < writes.size() && writes[next].clock <= clock) {
        psg.write(writes[next].clock, writes[next].reg, writes[next].value);
        ++next;
      }
      psg.drain(clock, window);
      for (int side = 0; side < 2; ++side) {
        double sum = 0;
        for (int index = 0; index < kChannels; ++index) {
          sum += window[index][side];
          soloed[index][side][sub] = window[index][side];
        }
        mixed[side][sub] = sum;
      }
    }
    for (int side = 0; side < 2; ++side) {
      for (int sub = 0; sub < kOversample; ++sub)
        mix_decimate[side].push(mix_filter[side].process(mixed[side][sub]));
      out.mix.push_back(yanes::replay::clip(mix_decimate[side].read() * scale));
    }
    for (int index = 0; index < kChannels; ++index)
      for (int side = 0; side < 2; ++side) {
        for (int sub = 0; sub < kOversample; ++sub)
          decimate[index][side].push(
              filter[index][side].process(soloed[index][side][sub]));
        out.solo[index].push_back(
            yanes::replay::clip(decimate[index][side].read() * scale));
      }
  }
  return out;
}

// The log's clock column counts PSG clocks from power-on and never restarts;
// a capture from a core built before that fix restarts it several times per
// frame, which would silently misplace most writes.
bool load(const std::string& path, std::vector<Write>& writes, int64_t& last) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot open " << path << "\n";
    return false;
  }
  std::string line;
  last = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    int64_t frame, clock;
    std::string reg, value;
    int selected;
    if (!(row >> frame >> clock >> reg >> value >> selected)) continue;
    if (clock < last) {
      std::cerr << "PSG log clock went backwards (" << clock << " after " << last
                << "): rebuild the core with third_party/"
                   "beetle-pce-psg-register-log.patch\n";
      return false;
    }
    last = clock;
    writes.push_back({clock, static_cast<uint8_t>(std::stoul(reg, nullptr, 16)),
                      static_cast<uint8_t>(std::stoul(value, nullptr, 16))});
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: yanes-pce-replay pce_psg.log out_dir [seconds]\n";
    return 2;
  }
  if (const char* revision = std::getenv("YANES_PCE_REVISION"))
    g_centred_dac = std::string(revision) == "huc6280a";

  std::vector<Write> writes;
  int64_t last = 0;
  if (!load(argv[1], writes, last)) return 1;

  const double seconds = argc >= 4 ? std::atof(argv[3]) : 0;
  const size_t frames = static_cast<size_t>(
      seconds > 0 ? seconds * kRate : last / kPsgClock * kRate + kRate);
  const std::string dir = argv[2];
  const Render out = render(writes, frames);
  yanes::replay::wav(dir + "/yanes_pce_mix.wav", out.mix, kRate);
  for (int index = 0; index < kChannels; ++index)
    yanes::replay::wav(dir + "/yanes_pce_ch" + std::to_string(index + 1) + ".wav",
                       out.solo[index], kRate);
  std::cout << "replayed " << writes.size() << " HuC6280 writes over "
            << frames / static_cast<double>(kRate) << "s\n";
}
