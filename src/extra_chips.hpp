// Models for the second wave of sound sources: sample players (Amiga Paula, GBA DirectSound,
// OKI ADPCM), one-bit speakers (ZX Spectrum, IBM PC, Apple II), the TMS5220-style LPC speech
// lattice, the Atari Lynx's programmable LFSR, the Virtual Boy and WonderSwan wavetables, the
// AY-3-8910's envelope used as an oscillator, and a slapped electric bass.
//
// Everything here is a clean-room model written from public descriptions of how the hardware
// works. None of it is copied from an emulator or a sample ROM; the built-in "samples" are
// generated from formulas.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace yanes::extra {

inline constexpr double kTau = 6.2831853071795864769;

// ----- Amiga Paula -------------------------------------------------------------------
//
// Paula plays 8-bit signed samples with no interpolation. Each channel fetches a new byte every
// `period` ticks of the PAL colour clock (3546895 Hz) and holds it until the next fetch, so the
// pitch can only land on clock / period, and the held steps image the sample around the replay
// rate. The A500 then runs the mix through a fixed RC lowpass (~4.4 kHz) and, with the power
// LED lit, a 12 dB/octave Butterworth "LED filter" at ~3.3 kHz.
inline constexpr double kPaulaClock = 3546895.0;
inline constexpr int kPaulaMinPeriod = 113;  // The fastest period ProTracker allows.
inline constexpr int kPaulaLoop = 32;        // Built-in loops are 32 bytes, like a tracker chip sample.

// The replay rate Paula actually achieves when asked for `rate` bytes per second.
inline double paula_rate(double rate) {
  const double period = std::clamp(std::round(kPaulaClock / std::max(1.0, rate)),
                                   static_cast<double>(kPaulaMinPeriod), 65535.0);
  return kPaulaClock / period;
}

// One byte of the built-in 32-byte loop for each shape: the short single-cycle waveforms
// MOD musicians drew by hand.
inline int8_t paula_builtin(int shape, int index) {
  const double p = (static_cast<double>(index & (kPaulaLoop - 1)) + 0.5) / kPaulaLoop;
  double w = 0.0;
  switch (std::clamp(shape, 0, 7)) {
    case 0: w = p < 0.5 ? 1.0 : -1.0; break;                                    // square
    case 1: w = 1.0 - 2.0 * p; break;                                           // saw
    case 2: w = 1.0 - 4.0 * std::abs(p - 0.5); break;                           // triangle
    case 3: w = p < 0.25 ? 1.0 : -1.0; break;                                   // 25% pulse
    case 4: w = std::sin(kTau * p); break;                                      // sine
    case 5: w = 0.6 * std::sin(kTau * p) + 0.4 * std::sin(3.0 * kTau * p); break;  // organ
    case 6: w = std::tanh(2.5 * (1.0 - 2.0 * p)) / std::tanh(2.5); break;       // fat bass
    default: w = (1.0 - 2.0 * p) + (index % 4 == 0 ? 0.45 : 0.0); break;        // fuzz saw
  }
  return static_cast<int8_t>(std::clamp(std::lround(w * 127.0), -128L, 127L));
}

// ----- one-bit speakers ----------------------------------------------------------------

// A pin-pulse engine's pulse: 64 ticks of a 3.58 MHz / 4 loop by default. Engines let the
// musician set that length; YANES offers 32, 64, 128 and 256 ticks.
inline constexpr double kZxTickSeconds = 1.0 / (3579545.0 / 4.0);
inline double zx_pulse_seconds(int duty_index) { return (32 << std::clamp(duty_index, 0, 3)) * kZxTickSeconds; }
// A ZX Spectrum beeper engine counts down a delay loop of `loop_tstates` Z80 cycles per step,
// so a pitch is a whole number of loop passes at 3.5 MHz.
inline double zx_frequency(double frequency) {
  constexpr double clock = 3500000.0, loop_tstates = 104.0;
  const double count = std::clamp(std::round(clock / (loop_tstates * frequency)), 1.0, 65535.0);
  return clock / (loop_tstates * count);
}
// The PC speaker is PIT channel 2 in square-wave mode: 1193182 Hz over a 16-bit divisor.
inline double pc_speaker_frequency(double frequency) {
  constexpr double clock = 1193182.0;
  const double divisor = std::clamp(std::round(clock / frequency), 1.0, 65535.0);
  return clock / divisor;
}
// The Apple II toggles its speaker from a 5-cycle delay loop at 1.023 MHz, once per half period.
inline double apple2_frequency(double frequency) {
  constexpr double clock = 1023000.0, loop_cycles = 5.0;
  const double loops = std::clamp(std::round(clock / (2.0 * frequency * loop_cycles)), 1.0, 65535.0);
  return clock / (2.0 * loops * loop_cycles);
}

// ----- AY-3-8910 envelope as an oscillator ("buzzer") -------------------------------------
//
// The AY's 16 volume steps go through a logarithmic DAC. Run the envelope generator at audio
// rate and it becomes a log-curved sawtooth or triangle: the Atari ST / Spectrum "buzzer" bass.
inline constexpr std::array<double, 16> kAyDac{
    0.0, 0.00999465934234, 0.0144502937362, 0.0210574502174, 0.0307011520562, 0.0455481803616,
    0.0644998855573, 0.107362478065, 0.126588845655, 0.20498970016, 0.292210269322,
    0.372838941024, 0.492530708782, 0.635324635691, 0.805584802014, 1.0};
inline constexpr double kAyDacMean = 0.263979;  // Mean of the 16 steps: a full ramp visits each once.

// Envelope step (0..15) for each envelope shape at `phase` through one envelope cycle.
// Shapes 0/1 are the repeating saws (registers 8 and 12), 2/3 the repeating triangles (10, 14).
inline int ay_envelope_step(int shape, double phase) {
  const double p = phase - std::floor(phase);
  switch (shape & 3) {
    case 0: return 15 - std::min(15, static_cast<int>(p * 16.0));
    case 1: return std::min(15, static_cast<int>(p * 16.0));
    case 2: { const int s = std::min(31, static_cast<int>(p * 32.0)); return s < 16 ? 15 - s : s - 16; }
    default: { const int s = std::min(31, static_cast<int>(p * 32.0)); return s < 16 ? s : 31 - s; }
  }
}
// The pitch an envelope period can reach: a saw cycle is 256 clocks per period count, a
// triangle (down then up) twice that.
inline double ay_envelope_frequency(double frequency, bool triangle) {
  constexpr double clock = 2000000.0;
  const double per_count = triangle ? 512.0 : 256.0;
  const double period = std::clamp(std::round(clock / (per_count * frequency)), 1.0, 65535.0);
  return clock / (per_count * period);
}

// ----- TMS5220-style LPC speech ---------------------------------------------------------
//
// The TI speech chips excite a ten-stage lattice filter with a short "chirp" for voiced sounds
// or noise for unvoiced ones, at 8 kHz. The lattice is driven by reflection coefficients
// (k1..k10) quantised to 5/5/4/4/4/4/4/3/3/3 bits. Here the coefficients come from formant
// targets rather than a speech ROM: each vowel's resonances are turned into a polynomial and
// stepped down into reflection coefficients, then quantised like the chip's.
inline constexpr int kLpcOrder = 10;
inline constexpr std::array<int8_t, 21> kChirp{
    0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50, 0x25, 0x26, 0x4c,
    0x44, 0x1a, 0x32, 0x3b, 0x13, 0x37, 0x1a, 0x25, 0x1f, 0x1d};
inline constexpr std::array<int, kLpcOrder> kLpcBits{5, 5, 4, 4, 4, 4, 4, 3, 3, 3};

struct Formants { double f[5]; };
// oo, oh, ah, eh, ee (adult male averages); the fourth and fifth poles shape the top end.
inline constexpr std::array<Formants, 5> kVowels{{
    {{300, 870, 2240, 3300, 3750}},
    {{450, 800, 2600, 3300, 3750}},
    {{730, 1090, 2440, 3300, 3750}},
    {{530, 1840, 2480, 3300, 3750}},
    {{270, 2290, 2900, 3400, 3800}},
}};
inline constexpr double kFormantBandwidth[5] = {70, 90, 130, 220, 300};

// Vowel formants at `position` 0..1 across oo → oh → ah → eh → ee.
inline Formants vowel_at(double position) {
  const double x = std::clamp(position, 0.0, 1.0) * 4.0;
  const int i = std::min(3, static_cast<int>(x));
  const double m = x - i;
  Formants out{};
  for (int n = 0; n < 5; ++n) out.f[n] = kVowels[i].f[n] * (1.0 - m) + kVowels[i + 1].f[n] * m;
  return out;
}

// Direct-form predictor A(z) = 1 + a1 z^-1 + ... + a10 z^-10 whose poles sit on the formants.
inline std::array<double, kLpcOrder + 1> formant_polynomial(const Formants& formants, double rate) {
  std::array<double, kLpcOrder + 1> a{};
  a[0] = 1.0;
  int order = 0;
  for (int n = 0; n < 5; ++n) {
    const double r = std::exp(-3.14159265358979323846 * kFormantBandwidth[n] / rate);
    const double c1 = -2.0 * r * std::cos(kTau * std::min(formants.f[n], rate * 0.48) / rate), c2 = r * r;
    for (int i = order + 2; i >= 0; --i) {
      double v = a[static_cast<size_t>(i)];
      if (i >= 1) v += c1 * a[static_cast<size_t>(i - 1)];
      if (i >= 2) v += c2 * a[static_cast<size_t>(i - 2)];
      if (i <= kLpcOrder) a[static_cast<size_t>(i)] = v;
    }
    order += 2;
  }
  return a;
}

// Step-down recursion: the lattice reflection coefficients of A(z). With this sign convention
// the synthesis lattice below has exactly the impulse response of 1 / A(z).
inline std::array<double, kLpcOrder> reflection_coefficients(std::array<double, kLpcOrder + 1> a) {
  std::array<double, kLpcOrder> k{};
  for (int m = kLpcOrder; m >= 1; --m) {
    const double km = std::clamp(a[static_cast<size_t>(m)], -0.9995, 0.9995);
    k[static_cast<size_t>(m - 1)] = km;
    std::array<double, kLpcOrder + 1> next = a;
    for (int i = 1; i < m; ++i)
      next[static_cast<size_t>(i)] = (a[static_cast<size_t>(i)] - km * a[static_cast<size_t>(m - i)]) / (1.0 - km * km);
    a = next;
  }
  return k;
}

// Quantise each coefficient to the chip's bit depth. The levels are spaced evenly in arcsine,
// which, like the chip's own tables, gives the most resolution near +-1 where the sharp low
// formants live.
inline std::array<double, kLpcOrder> quantize_reflections(const std::array<double, kLpcOrder>& k) {
  std::array<double, kLpcOrder> q{};
  for (int i = 0; i < kLpcOrder; ++i) {
    const int levels = 1 << kLpcBits[static_cast<size_t>(i)];
    const double limit = std::asin(0.995);
    const double theta = std::asin(std::clamp(k[static_cast<size_t>(i)], -0.995, 0.995));
    const double step = 2.0 * limit / (levels - 1);
    q[static_cast<size_t>(i)] = std::sin(-limit + std::round((theta + limit) / step) * step);
  }
  return q;
}

// Ten-stage all-pole lattice plus the chirp/noise excitation, run at its own sample rate.
struct LpcVoice {
  std::array<double, kLpcOrder> k{};
  std::array<double, kLpcOrder + 1> b{};  // Backward-path delays.
  double gain{};                          // sqrt(prod(1 - k^2)): normalises the lattice's resonance gain.
  int chirp{static_cast<int>(kChirp.size())};
  double pitch_phase{};
  uint32_t noise{0x1abcd};

  void set_formants(const Formants& formants, double rate, bool quantize) {
    k = reflection_coefficients(formant_polynomial(formants, rate));
    if (quantize) k = quantize_reflections(k);
    double g = 1.0;
    for (const double c : k) g *= 1.0 - c * c;
    gain = std::sqrt(std::max(1.0e-9, g));
  }
  double lattice(double excitation) {
    double f = excitation;
    for (int m = kLpcOrder; m >= 1; --m) {
      f -= k[static_cast<size_t>(m - 1)] * b[static_cast<size_t>(m - 1)];
      b[static_cast<size_t>(m)] = b[static_cast<size_t>(m - 1)] + k[static_cast<size_t>(m - 1)] * f;
    }
    b[0] = f;
    return f;
  }
  // One sample. `voicing` 1 is a pure chirp train at `pitch_hz`, 0 pure noise.
  double tick(double pitch_hz, double rate, double voicing) {
    double excitation = 0.0;
    if (voicing > 0.0) {
      pitch_phase += pitch_hz / rate;
      if (pitch_phase >= 1.0) { pitch_phase -= std::floor(pitch_phase); chirp = 0; }
      if (chirp < static_cast<int>(kChirp.size())) excitation += voicing * kChirp[static_cast<size_t>(chirp++)] / 64.0;
    }
    if (voicing < 1.0) {
      noise = noise & 1U ? (noise >> 1U) ^ 0x10004U : noise >> 1U;  // 17-bit LFSR
      excitation += (1.0 - voicing) * ((noise & 1U) ? 0.55 : -0.55);
    }
    return lattice(excitation * gain);
  }
};

// ----- OKI MSM6295 ADPCM ----------------------------------------------------------------
//
// 4-bit ADPCM (the Dialogic/OKI flavour) on a 12-bit signal: each nibble moves the signal by a
// multiple of a step size that grows on big nibbles and shrinks on small ones. Typical boards
// clock the chip at 1 MHz / 132, so samples replay at a fixed 7575 Hz with no pitch control.
inline constexpr double kOkiRate = 1000000.0 / 132.0;
inline constexpr std::array<int16_t, 49> kOkiSteps{
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552};
inline constexpr std::array<int8_t, 8> kOkiIndexShift{-1, -1, -1, -1, 2, 4, 6, 8};

struct OkiAdpcm {
  int signal{};
  int index{};
  void decode(int nibble) {
    const int step = kOkiSteps[static_cast<size_t>(index)];
    const int magnitude = ((2 * (nibble & 7) + 1) * step) / 8;
    signal = std::clamp(signal + ((nibble & 8) ? -magnitude : magnitude), -2048, 2047);
    index = std::clamp(index + kOkiIndexShift[static_cast<size_t>(nibble & 7)], 0, 48);
  }
  // Encode one 12-bit target and decode the result, as playing a pre-encoded stream would.
  int encode(int target) {
    const int step = kOkiSteps[static_cast<size_t>(index)];
    int diff = target - signal;
    int nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    nibble |= std::min(7, diff * 4 / step);
    decode(nibble);
    return nibble;
  }
};

// The built-in sounds the ADPCM voice plays without a loaded sample, the kind of thing an
// arcade board kept in its sample ROM: nine drums and three shouts. Index by key within an
// octave: kick, snare, clap, closed hat, low tom, open hat, mid tom, crash, high tom, "ha!",
// "hey!", "yeah!".
inline constexpr std::array<double, 12> kArcadeKitLength{0.42, 0.30, 0.30, 0.08, 0.46, 0.38, 0.42, 0.85, 0.36, 0.34, 0.38, 0.46};

// One sample (at the OKI rate) of sound `sound`, `t` seconds in. `lfsr` and `previous` carry the
// noise source and its last value (for the high-passed cymbals), `speech` the shouts' lattice.
inline double arcade_kit(int sound, double t, uint32_t& lfsr, double& previous, LpcVoice& speech) {
  lfsr = lfsr & 1U ? (lfsr >> 1U) ^ 0xb400U : lfsr >> 1U;  // 16-bit Galois LFSR
  if (lfsr == 0) lfsr = 1;
  const double noise = (lfsr & 1U) ? 1.0 : -1.0;
  const double bright = noise - previous;  // First difference: the hiss cymbals are made of.
  previous = noise;
  auto drop = [t](double from, double to, double speed) {  // Cycles of a sine gliding from -> to.
    return to * t + (from - to) * (1.0 - std::exp(-t * speed)) / speed;
  };
  switch (sound) {
    case 0: return std::sin(kTau * drop(160.0, 48.0, 28.0)) * std::exp(-t * 8.0) + 0.3 * noise * std::exp(-t * 300.0);
    case 1: return 0.65 * noise * std::exp(-t * 15.0) + 0.45 * std::sin(kTau * drop(240.0, 185.0, 30.0)) * std::exp(-t * 22.0);
    case 2: {
      double env = 0.0;
      for (const double onset : {0.0, 0.011, 0.022}) if (t >= onset) env = std::max(env, std::exp(-(t - onset) * 140.0));
      if (t >= 0.033) env = std::max(env, 0.55 * std::exp(-(t - 0.033) * 18.0));
      return 0.5 * (noise + bright) * env;
    }
    case 3: return 0.6 * bright * std::exp(-t * 55.0);
    case 4: return std::sin(kTau * drop(130.0, 82.0, 16.0)) * std::exp(-t * 7.5);
    case 5: return 0.55 * bright * std::exp(-t * 8.0);
    case 6: return std::sin(kTau * drop(175.0, 115.0, 16.0)) * std::exp(-t * 8.0);
    case 7: return 0.6 * (0.7 * bright + 0.3 * noise) * std::exp(-t * 4.8);
    case 8: return std::sin(kTau * drop(230.0, 160.0, 16.0)) * std::exp(-t * 9.0);
    default: break;
  }
  // Shouts: aspiration noise through the vowel's formants, then a chirp-voiced vowel whose pitch
  // falls as the shout dies away.
  struct Shout { double noise_end, from, to, pitch_from, pitch_to; };
  constexpr Shout shouts[3] = {{0.06, 0.5, 0.5, 190.0, 145.0},   // "ha!"   (ah)
                               {0.05, 0.75, 1.0, 215.0, 175.0},  // "hey!"  (eh -> ee)
                               {0.0, 1.0, 0.5, 175.0, 125.0}};   // "yeah!" (ee -> eh -> ah)
  const Shout& s = shouts[std::clamp(sound - 9, 0, 2)];
  const double length = kArcadeKitLength[static_cast<size_t>(std::clamp(sound, 0, 11))];
  const double x = std::clamp(t / length, 0.0, 1.0);
  const long tick = std::lround(t * kOkiRate);
  if (tick % 16 == 0) speech.set_formants(vowel_at(s.from + (s.to - s.from) * x), kOkiRate, true);
  const bool voiced = t >= s.noise_end;
  const double envelope = std::min(1.0, t / 0.012) * (voiced ? std::exp(-(t - s.noise_end) * 5.5) : 0.45 + 0.55 * t / std::max(1e-3, s.noise_end));
  return 0.9 * envelope * speech.tick(s.pitch_from + (s.pitch_to - s.pitch_from) * x, kOkiRate, voiced ? 1.0 : 0.0);
}

// ----- Atari Lynx (Mikey) ---------------------------------------------------------------
//
// Each Mikey channel is a 12-bit shift register whose feedback is the inverted XOR of a
// programmable set of taps, clocked by a timer. Short tap sets loop after a handful of steps
// and play as buzzy tones; long ones are noise. A shape picks one tap set.
inline constexpr std::array<uint16_t, 8> kLynxTaps{0x001, 0x007, 0x005, 0x024, 0x009, 0x012, 0x081, 0x801};
// The loop length each tap set settles into from a cleared register; the last one is noise,
// which is clocked as if it looped every 16 steps.
inline constexpr std::array<int, 8> kLynxPeriods{2, 4, 7, 9, 15, 31, 63, 16};

inline uint16_t lynx_lfsr_clock(uint16_t state, uint16_t taps, bool& out) {
  unsigned parity = static_cast<unsigned>(state & taps);
  parity ^= parity >> 8U; parity ^= parity >> 4U; parity ^= parity >> 2U; parity ^= parity >> 1U;
  const uint16_t feedback = static_cast<uint16_t>((parity & 1U) ^ 1U);
  out = feedback != 0;
  return static_cast<uint16_t>(((state << 1U) | feedback) & 0xfffU);
}
// The rate Mikey's timer can actually clock at: 1 MHz divided by a power of two (prescale 0..6)
// and a reload count of 1..256.
inline double lynx_timer_rate(double rate) {
  for (int prescale = 0; prescale <= 6; ++prescale) {
    const double clock = 1000000.0 / static_cast<double>(1 << prescale);
    const double count = std::round(clock / std::max(1.0, rate));
    if (count <= 256.0 || prescale == 6) return clock / std::clamp(count, 1.0, 256.0);
  }
  return rate;
}

// ----- Virtual Boy VSU and WonderSwan ---------------------------------------------------
//
// Both play 32-step wavetables from an 11-bit frequency register: the VSU with 6-bit samples
// off a 5 MHz clock, the WonderSwan with 4-bit samples off 3.072 MHz.
inline double wavetable_register_frequency(double frequency, double clock) {
  const double distance = std::clamp(std::round(clock / (32.0 * frequency)), 1.0, 2048.0);
  return clock / (32.0 * distance);
}
inline double vsu_frequency(double frequency) { return wavetable_register_frequency(frequency, 5000000.0); }
inline double wonderswan_frequency(double frequency) { return wavetable_register_frequency(frequency, 3072000.0); }

// The 11-bit register value that plays `frequency` (the distance below 2048 the counter reloads).
inline int wavetable_register(double frequency, double clock) {
  return 2048 - static_cast<int>(std::clamp(std::round(clock / (32.0 * frequency)), 1.0, 2048.0));
}

// Both consoles' noise is a 15-bit LFSR fed with NOT(bit 7 XOR one selectable tap). The VSU's
// noise channel steps once per ten counter reloads, the WonderSwan's once per wave step, so it
// tracks the frequency register like a tone would.
inline constexpr std::array<int, 8> kVsuNoiseTaps{14, 10, 13, 4, 8, 6, 9, 11};
inline constexpr std::array<int, 8> kSwanNoiseTaps{14, 10, 13, 4, 8, 6, 9, 11};
inline uint32_t console_noise_clock(uint32_t lfsr, int tap) {
  const uint32_t feedback = 1U ^ ((lfsr >> 7U) & 1U) ^ ((lfsr >> static_cast<unsigned>(tap)) & 1U);
  return ((lfsr << 1U) | feedback) & 0x7fffU;
}
// LFSR steps per second for a note: the VSU noise counter reloads with ten times the distance.
inline double vsu_noise_rate(double frequency) {
  return 5000000.0 / (10.0 * (2048 - wavetable_register(frequency, 5000000.0)));
}
inline double swan_noise_rate(double frequency) {
  return 3072000.0 / (2048 - wavetable_register(frequency, 3072000.0));
}

// VSU channel 5 modulation: every `interval` ticks of the 1041.67 Hz effects clock the next of
// 32 signed table entries is added to the channel's frequency register. YANES fills the table
// with a triangle `depth` register units deep, which on the hardware is a vibrato whose width
// depends on the note (the same depth moves a high note's short counter much further).
inline constexpr double kVsuEffectsClock = 5000000.0 / 4800.0;
inline double vsu_modulated_frequency(double frequency, double depth, double seconds, int interval = 4) {
  const int base = wavetable_register(frequency, 5000000.0);
  const long step = static_cast<long>(std::floor(seconds * kVsuEffectsClock / interval));
  const int position = static_cast<int>(step % 32);
  const double tri = position < 16 ? position / 8.0 - 1.0 : 3.0 - position / 8.0;
  const int entry = static_cast<int>(std::clamp(std::lround(tri * depth), -128L, 127L));
  const int register_value = std::clamp(base + entry, 0, 2047);
  return 5000000.0 / (32.0 * (2048 - register_value));
}

// WonderSwan channel 3 sweep: every 8192 * (ticks + 1) cycles the signed `amount` is added to the
// frequency register, wrapping at 11 bits, so the pitch climbs ever faster and then wraps.
inline double swan_swept_frequency(double frequency, int amount, int ticks, double seconds) {
  const int base = wavetable_register(frequency, 3072000.0);
  const long steps = static_cast<long>(std::floor(seconds * 3072000.0 / (8192.0 * (ticks + 1))));
  const int register_value = static_cast<int>(((base + steps * amount) % 2048 + 2048) % 2048);
  return 3072000.0 / (32.0 * (2048 - register_value));
}

// The 32-step table for each shape, as a bipolar value quantised to `levels`. Shape 7 is the
// plain rising ramp a freshly reset wavetable holds (and the voices' default, as for the FDS,
// N163, SCC and PC Engine); the WonderSwan's tables 0-6 come in a different order from the VSU's.
inline float console_wavetable(int shape, double phase, int levels, bool wonderswan) {
  const double p = std::floor((phase - std::floor(phase)) * 32.0) / 32.0;
  const int s = std::clamp(shape, 0, 7);
  if (s == 7) return static_cast<float>(std::floor(p * levels) / (levels - 1) * 2.0 - 1.0);
  double w = 0.0;
  switch (wonderswan ? (s + 4) % 7 : s) {
    case 0: w = 0.75 * std::sin(kTau * p) + 0.25 * std::sin(2.0 * kTau * p + 0.6); break;  // organ
    case 1: w = 1.0 - 4.0 * std::abs(p - 0.5); break;                                      // triangle
    case 2: w = std::sin(kTau * p); break;                                                  // sine
    case 3: w = 0.5 * std::sin(kTau * p) + 0.5 * std::sin(4.0 * kTau * p); break;           // bell
    case 4: w = p < 0.375 ? 1.0 : -0.6; break;                                              // buzz pulse
    case 5: w = 1.0 - 2.0 * p; break;                                                       // falling saw
    default: w = std::sin(kTau * p) * (p < 0.5 ? 1.0 : 0.35); break;                        // half-rectified
  }
  const double normalized = std::clamp(w * 0.5 + 0.5, 0.0, 1.0);
  return static_cast<float>(std::round(normalized * (levels - 1)) / (levels - 1) * 2.0 - 1.0);
}

// ----- GBA DirectSound ------------------------------------------------------------------
//
// The GBA's sound driver mixes 8-bit samples in software at a low fixed rate (13379 Hz is the
// common choice) by stepping through each sample with no interpolation, and updates volumes once
// per video frame. Built-in single-cycle loops are 64 bytes.
inline constexpr double kGbaMixRate = 13379.0;
inline constexpr double kGbaFrameRate = 59.7275;
inline constexpr int kGbaLoop = 64;
inline int8_t gba_builtin(int shape, int index) {
  const double p = (static_cast<double>(index & (kGbaLoop - 1)) + 0.5) / kGbaLoop;
  double w = 0.0;
  switch (std::clamp(shape, 0, 7)) {
    case 0: w = p < 0.5 ? 0.9 : -0.9; break;                                            // square
    case 1: w = 1.0 - 2.0 * p; break;                                                   // saw
    case 2: w = 0.7 * std::sin(kTau * p) + 0.3 * std::sin(2.0 * kTau * p) + 0.15 * std::sin(5.0 * kTau * p); break;  // piano-ish
    case 3: { double s = 0; for (int h = 1; h <= 9; ++h) s += std::sin(h * kTau * p) / (h * 1.3); w = s * 0.62; break; }  // strings
    case 4: w = std::sin(kTau * p) + 0.08 * std::sin(3.0 * kTau * p); break;            // flute
    case 5: w = 0.5 * std::sin(kTau * p) + 0.3 * std::sin(2.0 * kTau * p) + 0.2 * std::sin(4.0 * kTau * p); break;  // organ
    case 6: w = p < 0.3 ? 0.95 : -0.55; break;                                          // brass pulse
    default: w = 0.6 * std::sin(kTau * p) + 0.4 * std::sin(3.0 * kTau * p + 1.1); break;  // choir
  }
  return static_cast<int8_t>(std::clamp(std::lround(w * 120.0), -128L, 127L));
}

// ----- slap bass -----------------------------------------------------------------------
//
// A thumb-slapped (or popped) electric bass string: a stiff string's slightly stretched
// partials, the upper ones dying in a few tens of milliseconds while the fundamental rings,
// plus the click of the string hitting the frets. `pop` (0..1) moves from a soft thumb to a
// hard pop: brighter, more fret click, faster brightness decay. `cycles` is the fundamental's
// unwrapped phase, `age` the note's age in seconds, `click_noise` a fresh noise sample.
inline double slap_bass(double cycles, double age, double pop, double click_noise) {
  constexpr double stiffness = 0.00025;
  double out = 0.0, norm = 0.0;
  for (int n = 1; n <= 14; ++n) {
    const double stretch = n * std::sqrt(1.0 + stiffness * n * n);
    const double start = std::pow(static_cast<double>(n), -(1.25 - 0.75 * pop));
    // The fundamental rings for seconds; each overtone above it dies faster, more so when popped.
    const double decay = 0.9 + (0.5 + 0.8 * pop) * std::pow(static_cast<double>(n - 1), 1.35);
    const double a = start * std::exp(-age * decay);
    out += a * std::sin(kTau * (cycles * stretch - std::floor(cycles * stretch)));
    norm += start;
  }
  const double click = click_noise * (0.35 + 0.65 * pop) * std::exp(-age * 380.0);
  return out / (norm * 0.55) + click;
}

}  // namespace yanes::extra
