#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace yanes {

// Cycle-stepped 2A03 core for the NES stack path. Clean-room: published NTSC
// timers, duty sequencers, LFSR, and nonlinear mixer. Host samples are produced
// by integrating CPU-rate mixer output and applying a band-limited correction
// whenever the DAC level changes inside a sample (blip-style delta), plus a mild
// AV-path low-pass. Solo NES voices keep the existing oscillators for Furnace.
struct NesApu {
  static constexpr double kNtscClock = 1789773.0;
  static constexpr double kPalClock = 1662607.0;
  static constexpr std::array<uint16_t, 16> kNoisePeriods{
      4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

  void set_clock(bool pal) { cpu_clock_ = pal ? kPalClock : kNtscClock; update_rate(); }
  void set_sample_rate(double rate) {
    sample_rate_ = std::max(8000.0, rate);
    update_rate();
  }

  void reset() {
    pulse_ = {};
    triangle_ = {};
    noise_ = {};
    noise_.shift = 1;
    sample_hold_ = 0.0;
    mix_acc_ = 0.0;
    mix_count_ = 0.0;
    last_mix_ = 0.0;
    blep_ = 0.0;
    lp_ = 0.0;
    dc_x_ = dc_y_ = 0.0;
  }

  void note_on(int channel, int key, int duty, double velocity) {
    if (channel < 0 || channel > 3) return;
    const int vol = std::clamp(static_cast<int>(std::lround(velocity * 15.0)), 0, 15);
    if (channel <= 1) {
      auto& p = pulse_[static_cast<size_t>(channel)];
      p.active = true;
      p.duty = static_cast<uint8_t>(std::clamp(duty, 0, 3));
      p.volume = static_cast<uint8_t>(vol);
      p.seq = 0;
      const double hz = midi_hz(key);
      p.timer = static_cast<uint16_t>(
          std::clamp(std::round(cpu_clock_ / (16.0 * hz) - 1.0), 8.0, 2047.0));
      p.counter = p.timer;
    } else if (channel == 2) {
      triangle_.active = true;
      triangle_.seq = 0;
      const double hz = midi_hz(key);
      triangle_.timer = static_cast<uint16_t>(
          std::clamp(std::round(cpu_clock_ / (32.0 * hz) - 1.0), 2.0, 2047.0));
      triangle_.counter = triangle_.timer;
    } else {
      noise_.active = true;
      noise_.volume = static_cast<uint8_t>(vol);
      const int idx = std::clamp(key - 48, 0, 15);
      noise_.period = kNoisePeriods[static_cast<size_t>(idx)];
      noise_.counter = noise_.period;
      noise_.short_mode = key >= 64;
    }
  }

  void note_off(int channel) {
    if (channel == 0 || channel == 1) pulse_[static_cast<size_t>(channel)].active = false;
    else if (channel == 2) triangle_.active = false;
    else if (channel == 3) noise_.active = false;
  }

  void set_pulse_duty(int channel, int duty) {
    if (channel == 0 || channel == 1)
      pulse_[static_cast<size_t>(channel)].duty =
          static_cast<uint8_t>(std::clamp(duty, 0, 3));
  }

  // mute_mask / solo_mask use bits 0..3 for pulse1, pulse2, triangle, noise.
  // dpcm is the 0..127 DPCM DAC level already muted/solo-filtered by the caller.
  float render(uint32_t mute_mask, uint32_t solo_mask, double dpcm_dac = 0.0) {
    const bool any_audible =
        (pulse_[0].active && audible(0, mute_mask, solo_mask)) ||
        (pulse_[1].active && audible(1, mute_mask, solo_mask)) ||
        (triangle_.active && audible(2, mute_mask, solo_mask)) ||
        (noise_.active && audible(3, mute_mask, solo_mask)) ||
        (dpcm_dac > 1.0e-9);

    const double target = clocks_per_sample_;
    while (sample_hold_ + 1.0 <= target + 1.0e-9) {
      step_cycle();
      if (!any_audible) {
        sample_hold_ += 1.0;
        continue;
      }
      const double raw = mix(mute_mask, solo_mask, dpcm_dac);
      if (raw != last_mix_) {
        const double frac = sample_hold_ / target;
        blep_ += (raw - last_mix_) * step_blep(frac);
        last_mix_ = raw;
      }
      mix_acc_ += raw;
      mix_count_ += 1.0;
      sample_hold_ += 1.0;
    }
    sample_hold_ -= target;

    if (!any_audible) {
      // Hard mute / all gates clear: exact silence, no LP/DC tail.
      mix_acc_ = 0.0;
      mix_count_ = 0.0;
      blep_ = 0.0;
      last_mix_ = 0.0;
      lp_ = 0.0;
      dc_x_ = dc_y_ = 0.0;
      return 0.0f;
    }

    const double averaged =
        (mix_count_ > 0.0 ? mix_acc_ / mix_count_ : last_mix_) + blep_;
    mix_acc_ = 0.0;
    mix_count_ = 0.0;
    blep_ = 0.0;

    lp_ += lp_alpha_ * (averaged - lp_);

    constexpr double kDc = 0.995;
    const double dc = kDc * (dc_y_ + lp_ - dc_x_);
    dc_x_ = lp_;
    dc_y_ = dc;

    return static_cast<float>(std::clamp(dc * 2.8, -1.0, 1.0));
  }

 private:
  struct Pulse {
    bool active{false};
    uint8_t duty{2};
    uint8_t volume{12};
    uint8_t seq{0};
    uint16_t timer{0};
    uint16_t counter{0};
  };
  struct Triangle {
    bool active{false};
    uint8_t seq{0};
    uint16_t timer{0};
    uint16_t counter{0};
  };
  struct Noise {
    bool active{false};
    bool short_mode{false};
    uint8_t volume{10};
    uint16_t period{4};
    uint16_t counter{0};
    uint16_t shift{1};
  };

  static double midi_hz(int key) {
    return 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
  }

  // PolyBLEP residual for a unit step that occurred at fractional delay `frac`
  // (0 = start of sample, 1 = end). Integrated against the boxcar sample period.
  static double step_blep(double frac) {
    frac = std::clamp(frac, 0.0, 1.0);
    // First-order polyBLEP antiderivative over the remaining fraction of the sample.
    const double t = 1.0 - frac;
    return 0.5 * t * t;
  }

  void update_rate() {
    clocks_per_sample_ = cpu_clock_ / sample_rate_;
    lp_alpha_ = 1.0 - std::exp(-6.28318530718 * 12000.0 / sample_rate_);
    dc_r_ = std::exp(-2.0 * 3.14159265358979323846 * 0.05 / sample_rate_);
    (void)dc_r_;
  }

  bool audible(int channel, uint32_t mute_mask, uint32_t solo_mask) const {
    const uint32_t bit = 1U << channel;
    if (mute_mask & bit) return false;
    if (solo_mask && !(solo_mask & bit)) return false;
    return true;
  }

  void step_cycle() {
    static constexpr uint8_t duty_table[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0},
        {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
    (void)duty_table;

    for (auto& p : pulse_) {
      if (!p.active) continue;
      if (p.counter > 0) --p.counter;
      else {
        p.counter = p.timer;
        p.seq = static_cast<uint8_t>((p.seq + 1) & 7);
      }
    }
    if (triangle_.active) {
      if (triangle_.counter > 0) --triangle_.counter;
      else {
        triangle_.counter = triangle_.timer;
        triangle_.seq = static_cast<uint8_t>((triangle_.seq + 1) & 31);
      }
    }
    if (noise_.active) {
      if (noise_.counter > 0) --noise_.counter;
      else {
        noise_.counter = noise_.period;
        const uint16_t tap = noise_.short_mode ? 6U : 1U;
        const uint16_t fb =
            static_cast<uint16_t>((noise_.shift & 1U) ^ ((noise_.shift >> tap) & 1U));
        noise_.shift = static_cast<uint16_t>((noise_.shift >> 1U) | (fb << 14U));
      }
    }
  }

  double mix(uint32_t mute_mask, uint32_t solo_mask, double dpcm_dac) const {
    static constexpr uint8_t duty_table[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0},
        {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
    static constexpr uint8_t tri_table[32] = {
        15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    double p1 = 0.0, p2 = 0.0, tr = 0.0, ns = 0.0;
    if (pulse_[0].active && audible(0, mute_mask, solo_mask) &&
        duty_table[pulse_[0].duty][pulse_[0].seq])
      p1 = pulse_[0].volume;
    if (pulse_[1].active && audible(1, mute_mask, solo_mask) &&
        duty_table[pulse_[1].duty][pulse_[1].seq])
      p2 = pulse_[1].volume;
    if (triangle_.active && audible(2, mute_mask, solo_mask))
      tr = tri_table[triangle_.seq];
    if (noise_.active && audible(3, mute_mask, solo_mask) && (noise_.shift & 1U) == 0)
      ns = noise_.volume;

    const double p_sum = p1 + p2;
    const double pulse_out =
        p_sum > 0.0 ? 95.88 / ((8128.0 / p_sum) + 100.0) : 0.0;
    const double tnd_sum =
        tr / 8227.0 + ns / 12241.0 + std::max(0.0, dpcm_dac) / 22638.0;
    const double tnd_out =
        tnd_sum > 0.0 ? 159.79 / ((1.0 / tnd_sum) + 100.0) : 0.0;
    return pulse_out + tnd_out;
  }

  std::array<Pulse, 2> pulse_{};
  Triangle triangle_{};
  Noise noise_{};
  double cpu_clock_{kNtscClock};
  double sample_rate_{48000.0};
  double clocks_per_sample_{kNtscClock / 48000.0};
  double sample_hold_{};
  double mix_acc_{};
  double mix_count_{};
  double last_mix_{};
  double blep_{};
  double lp_{};
  double lp_alpha_{};
  double dc_x_{}, dc_y_{}, dc_r_{};
};

}  // namespace yanes
