// Shared render plumbing for the hardware-oracle replay tools.
//
// All three replays face the same problem: the chip they model emits a
// piecewise-constant waveform whose steps run at the master clock, while the
// oracle they are scored against band-limits that waveform before producing
// host-rate audio. Sampling the chip once per output sample instead folds every
// step edge back into the audible band, which shows up as a spectral mismatch
// that no amount of register-level accuracy can remove.
//
// So each replay integrates its channels exactly over a window several times
// shorter than an output sample, then decimates through a windowed-sinc filter.
#ifndef YANES_TOOLS_REPLAY_SUPPORT_HPP
#define YANES_TOOLS_REPLAY_SUPPORT_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace yanes::replay {

// Exact area under one channel's output: the level is held between events, so
// each span contributes level * duration and no edge is ever missed.
struct Integrator {
  int64_t last_change{0};
  double level[2]{0, 0};
  double integral[2]{0, 0};

  void hold_until(int64_t clock) {
    if (clock <= last_change) return;
    const double span = static_cast<double>(clock - last_change);
    integral[0] += level[0] * span;
    integral[1] += level[1] * span;
    last_change = clock;
  }
  // Mean level over a window of `span` master clocks, rearmed for the next one.
  double mean(int side, double span) {
    const double value = span > 0 ? integral[side] / span : level[side];
    integral[side] = 0;
    return value;
  }
};

struct OnePole {
  double state{0};
  double alpha{1};
  void cutoff(double hz, double rate) {
    alpha = 1.0 - std::exp(-2.0 * M_PI * hz / rate);
  }
  double low(double x) {
    state += (x - state) * alpha;
    return state;
  }
  double high(double x) {
    state += (x - state) * alpha;
    return x - state;
  }
};

// Windowed-sinc decimator. The oversampled stream carries chip steps well past
// 20 kHz and folding those down would colour a spectrum comparison.
template <int Oversample>
struct Decimator {
  static constexpr int kTaps = Oversample * 32 + 1;
  std::array<double, kTaps> coefficient{};
  std::array<double, kTaps> history{};
  size_t cursor{0};

  Decimator() {
    const double band = 0.45 / Oversample;  // Cycles per oversampled sample.
    double sum = 0;
    for (int i = 0; i < kTaps; ++i) {
      const double x = i - (kTaps - 1) / 2.0;
      const double sinc =
          x == 0.0 ? 2.0 * band : std::sin(2.0 * M_PI * band * x) / (M_PI * x);
      const double window = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / (kTaps - 1));
      coefficient[i] = sinc * window;
      sum += coefficient[i];
    }
    for (double& c : coefficient) c /= sum;
  }
  void push(double x) {
    history[cursor] = x;
    cursor = static_cast<size_t>((cursor + 1) % kTaps);
  }
  double read() const {
    double sum = 0;
    for (int i = 0; i < kTaps; ++i)
      sum += coefficient[i] * history[(cursor + i) % kTaps];
    return sum;
  }
};

inline void wav(const std::string& path, const std::vector<int16_t>& pcm,
                uint32_t rate) {
  std::ofstream out(path, std::ios::binary);
  const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2), riff = 36 + bytes;
  const uint32_t fmt_size = 16, byte_rate = rate * 4;
  const uint16_t format = 1, channels = 2, align = 4, bits = 16;
  auto w = [&](const void* p, size_t n) {
    out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
  };
  w("RIFF", 4); w(&riff, 4); w("WAVEfmt ", 8); w(&fmt_size, 4); w(&format, 2);
  w(&channels, 2); w(&rate, 4); w(&byte_rate, 4); w(&align, 2); w(&bits, 2);
  w("data", 4); w(&bytes, 4); w(pcm.data(), bytes);
}

inline int16_t clip(double value) {
  return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
}

}  // namespace yanes::replay

#endif  // YANES_TOOLS_REPLAY_SUPPORT_HPP
