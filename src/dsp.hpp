#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace yanes {

inline constexpr double kCpuClock = 1789773.0;
inline constexpr std::array<int, 16> kNoisePeriods{
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
inline constexpr std::array<int, 16> kDpcmPeriods{
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 85, 72, 54};
inline constexpr std::array<int, 16> kDpcmPeriodsPal{
    398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50};

inline double midi_frequency(double note) {
  return 440.0 * std::exp2((note - 69.0) / 12.0);
}

// The NES sums the triangle, noise and DPCM through one non-linear DAC. A
// two-level channel comes out of it unchanged apart from scale, but the
// triangle's staircase is bent, and that bend is what puts even harmonics into
// an otherwise symmetric waveform.
inline float nes_tnd_shape(float bipolar) {
  const double level =
      (std::clamp(static_cast<double>(bipolar), -1.0, 1.0) * 0.5 + 0.5) * 15.0;
  constexpr double full = 159.79 / (8227.0 / 15.0 + 100.0);
  const double out = level > 0.0 ? 159.79 / (8227.0 / level + 100.0) : 0.0;
  return static_cast<float>(out * 2.0 / full - 1.0);
}

// The NES noise channel has no divider: the note picks one of the sixteen
// period-table entries, one entry per semitone, and wraps back to the top of
// the table every sixteen semitones the way the register itself does.
inline int nes_noise_index(int base_period, int semitones_from_base) {
  const int index = (base_period - semitones_from_base) % 16;
  return index < 0 ? index + 16 : index;
}

// Game Boy NR43 splits the shift rate into a 4-bit octave and a 3-bit divisor,
// so the note grid runs four steps to the octave: the divisor supplies 7/8, 3/4
// and 5/8 of the octave step and then the shift field halves the rate.
inline double game_boy_noise_hz(double base_hz, int semitones_from_base) {
  constexpr std::array<double, 4> kDivisors{1.0, 7.0 / 8.0, 3.0 / 4.0, 5.0 / 8.0};
  const int octave = static_cast<int>(
      std::floor(static_cast<double>(semitones_from_base) / 4.0));
  const size_t step = static_cast<size_t>(semitones_from_base - octave * 4);
  // Clamped to what the register can actually express: divisor 0 with shift 0 at
  // the top, divisor 7 with shift 13 at the bottom.
  return std::clamp(base_hz * std::exp2(octave) / kDivisors[step],
                    262144.0 / 7.0 / 16384.0, 262144.0);
}

inline float poly_blep(double phase, double increment) {
  if (increment <= 0.0) return 0.0f;
  if (phase < increment) {
    const double x = phase / increment;
    return static_cast<float>(x + x - x * x - 1.0);
  }
  if (phase > 1.0 - increment) {
    const double x = (phase - 1.0) / increment;
    return static_cast<float>(x * x + x + x + 1.0);
  }
  return 0.0f;
}

inline float pulse(double phase, double increment, double duty) {
  float value = phase < duty ? 1.0f : -1.0f;
  value += poly_blep(phase, increment);
  double falling = phase - duty;
  if (falling < 0.0) falling += 1.0;
  value -= poly_blep(falling, increment);
  return value;
}

// Untreated 2A03 pulse: hard edges at the sequencer duty. Use for NES stack /
// Strict Hardware so the nonlinear mixer sees the same two-level DAC the chip does.
inline float pulse_raw(double phase, double duty) {
  return phase < duty ? 1.0f : -1.0f;
}

inline float nes_triangle(double phase) {
  // The 2A03 sequencer has 32 steps: 15..0, 0..15.
  const int step = std::clamp(static_cast<int>(phase * 32.0), 0, 31);
  const int level = step < 16 ? 15 - step : step - 16;
  return static_cast<float>((static_cast<double>(level) / 15.0) * 2.0 - 1.0);
}

inline float quantize_bipolar(double value, int levels) {
  const double normalized = std::clamp(value * 0.5 + 0.5, 0.0, 1.0);
  const double quantized = std::round(normalized * (levels - 1)) / (levels - 1);
  return static_cast<float>(quantized * 2.0 - 1.0);
}

inline float vrc6_saw(double phase, int accumulator_step) {
  // The VRC6 saw adds its 6-bit rate into an 8-bit accumulator once every two
  // of the 14 steps in its sequence, so a cycle is seven held levels, and puts
  // the accumulator's top five bits on the DAC. A high rate overflows the
  // accumulator part-way through, which is what folds the ramp back on itself
  // instead of producing a clean saw.
  const int step = std::clamp(static_cast<int>(phase * 14.0), 0, 13);
  const int additions = step / 2;
  const int accumulator = (additions * std::clamp(accumulator_step, 0, 63)) & 0xff;
  return static_cast<float>((accumulator >> 3) / 15.5 - 1.0);
}

inline float fds_wave(double phase, int shape) {
  constexpr double tau = 6.2831853071795864769;
  const double p = std::floor(phase * 64.0) / 64.0;
  // The top of the range is the plain ramp a wavetable chip holds after a
  // reset, which is what the hardware reference renders play.
  if (std::clamp(shape, 0, 7) == 7)
    return quantize_bipolar(2.0 * p - 1.0, 64);
  const double harmonic = (std::clamp(shape, 0, 7) - 3.5) / 14.0;
  return quantize_bipolar(std::sin(tau * p) + harmonic * std::sin(tau * 2.0 * p), 64);
}

inline float n163_wave(double phase, int shape) {
  constexpr double tau = 6.2831853071795864769;
  const int length = 4 * (std::clamp(shape, 0, 7) + 1);
  const double p = std::floor(phase * length) / length;
  // Longest table doubles as the plain ramp a wavetable chip holds after a
  // reset, which is what the hardware reference renders play.
  if (std::clamp(shape, 0, 7) == 7)
    return quantize_bipolar(2.0 * p - 1.0, 16);
  const double wave = 0.72 * std::sin(tau * p) + 0.28 * std::sin(tau * 3.0 * p);
  return quantize_bipolar(wave, 16);
}

inline float vrc7_fm(double phase, double ratio, double index) {
  constexpr double tau = 6.2831853071795864769;
  const double modulator = std::sin(tau * std::fmod(phase * ratio, 1.0));
  return static_cast<float>(std::sin(tau * phase + index * modulator));
}

inline float genesis_fm(double phase, int algorithm, double feedback) {
  constexpr double tau = 6.2831853071795864769;
  const double p = tau * phase;
  const double fb = std::clamp(feedback, 0.0, 7.0) * 0.18;
  const double a = std::sin(p + fb * std::sin(p));
  const double b = std::sin(p * 2.0 + 2.2 * a);
  const double c = std::sin(p * 3.0 + 1.8 * (algorithm < 4 ? b : a));
  const double d = std::sin(p + 1.6 * (algorithm < 2 ? c : b));
  switch (std::clamp(algorithm, 0, 7)) {
    case 0: return static_cast<float>(d);                   // 1→2→3→4
    case 1: return static_cast<float>(0.65 * d + 0.35 * c);
    case 2: return static_cast<float>(0.55 * d + 0.45 * b);
    case 3: return static_cast<float>((c + d) * 0.5);
    case 4: return static_cast<float>((b + std::sin(p + 1.5 * c)) * 0.5);
    case 5: return static_cast<float>((a + b + c) / 3.0);
    case 6: return static_cast<float>((a + c + d) / 3.0);
    default: return static_cast<float>((a + b + c + d) * 0.25);
  }
}

inline uint32_t lfsr_clock(uint32_t state, unsigned tap, unsigned width) {
  const uint32_t feedback = (state & 1U) ^ ((state >> tap) & 1U);
  const uint32_t mask = (1U << width) - 1U;
  return ((state >> 1U) | (feedback << (width - 1U))) & mask;
}

// DMG CH4 feeds bit0 XOR bit1 back into bit 14, and narrow mode mirrors that
// same bit into bit 6 to shorten the period. The feedback must be XOR, not XNOR:
// an XNOR register is absorbed by the all-ones state, which is exactly the value
// the hardware loads on trigger, so the sequence would freeze into a DC level
// the moment a real ROM started a noise note.
inline uint32_t game_boy_lfsr_clock(uint32_t state, bool narrow) {
  const uint32_t feedback = (state ^ (state >> 1U)) & 1U;
  state = (state >> 1U) | (feedback << 14U);
  if (narrow) state = (state & ~(1U << 6U)) | (feedback << 6U);
  return state & 0x7fffU;
}

// Game Boy CH3 plays 32 four-bit samples straight out of wave RAM (FF30-FF3F,
// high nibble first). Unlike the shaped wavetables above this is not a
// parameterized family: a register-driven render has to play back exactly the
// bytes the sound driver wrote, so the table is an input rather than a shape id.
inline float game_boy_wave_sample(double phase, const uint8_t* wave_ram) {
  const int step = std::clamp(static_cast<int>(phase * 32.0), 0, 31);
  const uint8_t byte = wave_ram[step >> 1];
  const int nibble = (step & 1) ? (byte & 0x0f) : (byte >> 4);
  return static_cast<float>(nibble / 7.5 - 1.0);
}

// NR32 attenuates CH3 by shifting the sample right: 0 mutes, 1 is full volume,
// 2 is half, 3 is a quarter. There is no finer step on the hardware.
inline float game_boy_wave_level(float sample, int volume_code) {
  switch (std::clamp(volume_code, 0, 3)) {
    case 0: return 0.0f;
    case 1: return sample;
    case 2: return sample * 0.5f;
    default: return sample * 0.25f;
  }
}

// HuC6280 channels play 32 unsigned five-bit wave-RAM samples. Register-driven
// ROM replay supplies the exact RAM image instead of approximating it with one
// of the musical shape controls used by the normal PCE preset.
inline float pce_wave_sample(double phase, const uint8_t* wave_ram) {
  const int step = std::clamp(static_cast<int>(phase * 32.0), 0, 31);
  return static_cast<float>((wave_ram[step] & 0x1f) / 15.5 - 1.0);
}

inline uint32_t sega_psg_lfsr_clock(uint32_t state, bool white_noise) {
  const uint32_t feedback = white_noise ? ((state ^ (state >> 3U)) & 1U) : (state & 1U);
  return (state >> 1U) | (feedback << 15U);
}

inline uint32_t pce_lfsr_clock(uint32_t state) {
  const uint32_t feedback = ((state >> 0U) ^ (state >> 1U) ^ (state >> 11U) ^
                             (state >> 12U) ^ (state >> 17U)) & 1U;
  return ((state >> 1U) | (feedback << 17U)) & 0x3ffffU;
}

inline float pce_wave(double phase, int shape) {
  constexpr double tau = 6.2831853071795864769;
  const double p = std::floor(phase * 32.0) / 32.0;
  const double blend = std::clamp(shape, 0, 7) / 7.0;
  const double wave = (1.0 - blend) * std::sin(tau * p) +
                      blend * (2.0 * p - 1.0) + 0.18 * std::sin(tau * 3.0 * p);
  return quantize_bipolar(wave, 32);
}

inline float opl_two_operator(double phase, double ratio, double index, double feedback) {
  constexpr double tau = 6.2831853071795864769;
  const double p = tau * phase;
  const double mod = std::sin(p * ratio + std::sin(p) * feedback * 0.12);
  return static_cast<float>(std::sin(p + mod * index));
}

inline float scc_wave(double phase, int shape) {
  constexpr double tau = 6.2831853071795864769;
  const double p = std::floor(phase * 32.0) / 32.0;
  // The top of the range is the plain ramp a wavetable chip holds after a
  // reset, which is what the hardware reference renders play.
  if (std::clamp(shape, 0, 7) == 7)
    return quantize_bipolar(2.0 * p - 1.0, 256);
  const double blend = std::clamp(shape, 0, 7) / 7.0;
  const double wave = (1.0 - blend) * std::sin(tau * p) + blend * std::sin(tau * 2.0 * p) +
                      0.22 * std::sin(tau * 5.0 * p);
  return quantize_bipolar(wave, 256);
}

inline float morph_wavetable(double phase, double position, double warp) {
  constexpr double tau = 6.2831853071795864769;
  const double bent = std::pow(phase, std::exp2((std::clamp(warp, 0.0, 1.0) - 0.5) * 3.0));
  const double sine = std::sin(tau * bent);
  const double triangle = 1.0 - 4.0 * std::abs(bent - 0.5);
  const double saw = 2.0 * bent - 1.0;
  const double square = bent < 0.5 ? 1.0 : -1.0;
  const double p = std::clamp(position, 0.0, 1.0) * 3.0;
  const int region = std::min(2, static_cast<int>(p));
  const double mix = p - region;
  const double waves[] = {sine, triangle, saw, square};
  return quantize_bipolar(waves[region] * (1.0 - mix) + waves[region + 1] * mix, 256);
}

inline float phase_distortion(double phase, double amount, double shape) {
  constexpr double tau = 6.2831853071795864769;
  const double breakpoint = 0.08 + 0.84 * std::clamp(shape, 0.0, 1.0);
  const double a = 0.5 + 0.49 * std::clamp(amount, 0.0, 1.0);
  const double distorted = phase < breakpoint
      ? phase * a / breakpoint
      : a + (phase - breakpoint) * (1.0 - a) / (1.0 - breakpoint);
  return static_cast<float>(std::cos(tau * distorted));
}

inline float additive(double phase, double tilt, double odd_even) {
  constexpr double tau = 6.2831853071795864769;
  double output = 0.0, weight = 0.0;
  for (int harmonic = 1; harmonic <= 12; ++harmonic) {
    const double parity = (harmonic & 1) ? 1.0 : std::clamp(odd_even, 0.0, 1.0);
    const double amplitude = parity / std::pow(static_cast<double>(harmonic), 0.35 + 2.65 * tilt);
    output += std::sin(tau * phase * harmonic) * amplitude;
    weight += amplitude;
  }
  return static_cast<float>(output / std::max(1.0, weight * 0.72));
}

inline float six_operator_fm(double phase, int algorithm, double index, double brightness) {
  constexpr double tau = 6.2831853071795864769;
  const double p = tau * phase;
  const double b = 0.25 + brightness * 0.75;
  const double o6 = std::sin(p * 6.0) * index * b;
  const double o5 = std::sin(p * 5.0 + o6) * index * 0.8;
  const double o4 = std::sin(p * 4.0 + ((algorithm & 1) ? o6 : o5)) * index * 0.65;
  const double o3 = std::sin(p * 3.0 + ((algorithm & 2) ? o5 : o4)) * index * 0.5;
  const double o2 = std::sin(p * 2.0 + ((algorithm & 4) ? o4 : o3)) * index * 0.4;
  const double carrier = std::sin(p + ((algorithm & 8) ? o3 + o2 : o2));
  if (algorithm & 16) return static_cast<float>((carrier + std::sin(p + o4) + std::sin(p * 2.0 + o6)) / 3.0);
  return static_cast<float>(carrier);
}

// Original, parameterized voices inspired by broad 1980s/90s instrument families.
// These deliberately avoid factory ROM data and proprietary preset parameters.
inline float porta_fm(double phase, double ratio, double index, double brightness) {
  constexpr double tau = 6.2831853071795864769;
  const double p = tau * phase;
  const double coarse_ratio = std::round(std::clamp(ratio, 0.5, 8.0) * 2.0) * 0.5;
  const double mod = std::sin(p * coarse_ratio + 0.18 * std::sin(p * coarse_ratio));
  const double carrier = std::sin(p + mod * index * (0.45 + brightness * 0.8));
  return quantize_bipolar(carrier * 0.9 + std::sin(p * 2.0) * brightness * 0.1, 1024);
}

inline float analog_poly(double phase, double auxiliary_phase, double shape) {
  const double blend = std::clamp(shape, 0.0, 1.0);
  const double saw_a = phase * 2.0 - 1.0;
  const double saw_b = auxiliary_phase * 2.0 - 1.0;
  const double pulse = auxiliary_phase < 0.48 ? 1.0 : -1.0;
  return static_cast<float>((saw_a * 0.52 + saw_b * 0.32 + pulse * 0.16) * (0.82 + blend * 0.18));
}

inline float digital_ensemble(double phase, double position) {
  constexpr double tau = 6.2831853071795864769;
  const double p = std::floor(phase * 128.0) / 128.0;
  const double bright = std::clamp(position, 0.0, 1.0);
  const double wave = std::sin(tau*p) + 0.34*std::sin(tau*p*2.01) +
                      bright*0.22*std::sin(tau*p*5.0) + 0.12*std::sin(tau*p*7.02);
  return quantize_bipolar(wave * 0.62, 256);
}

inline float tine_piano(double phase, double index, double brightness, double age_seconds) {
  constexpr double tau = 6.2831853071795864769;
  const double p = tau * phase;
  const double strike = std::exp(-age_seconds * (4.0 + brightness * 5.0));
  const double mod = std::sin(p * 3.0) * index * (0.16 + strike * 0.28);
  return static_cast<float>(std::sin(p + mod) * 0.82 + std::sin(p * 2.0) * strike * 0.18);
}

struct NoiseLfsr {
  uint16_t bits{1};

  void reset(uint16_t seed = 1) { bits = static_cast<uint16_t>(seed & 0x7fff); if (bits == 0) bits = 1; }
  float clock(bool short_mode) {
    const unsigned tap = short_mode ? 6U : 1U;
    const uint16_t feedback = static_cast<uint16_t>((bits & 1U) ^ ((bits >> tap) & 1U));
    bits = static_cast<uint16_t>((bits >> 1U) | (feedback << 14U));
    return (bits & 1U) ? -1.0f : 1.0f;
  }
};

}  // namespace yanes
