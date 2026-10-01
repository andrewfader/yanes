#include "../src/dsp.hpp"
#include "../src/snes_dsp.hpp"
#include <cassert>
#include <cmath>
#include <set>
#include <vector>

int main() {
  assert(std::abs(yanes::midi_frequency(69.0) - 440.0) < 1e-10);
  assert(std::abs(yanes::midi_frequency(60.0) - 261.625565) < 1e-5);

  std::set<uint16_t> long_states;
  yanes::NoiseLfsr noise;
  for (int i = 0; i < 32767; ++i) {
    assert(long_states.insert(noise.bits).second);
    noise.clock(false);
  }
  assert(noise.bits == 1);

  std::set<uint16_t> short_states;
  noise.reset();
  do {
    assert(short_states.insert(noise.bits).second);
    noise.clock(true);
  } while (noise.bits != 1);
  assert(short_states.size() == 93);

  for (int i = 0; i < 32; ++i) {
    const float value = yanes::nes_triangle((i + 0.5) / 32.0);
    assert(value >= -1.0f && value <= 1.0f);
  }

  for (int shape = 0; shape < 8; ++shape) {
    for (int i = 0; i < 64; ++i) {
      const double phase = (i + 0.5) / 64.0;
      assert(std::isfinite(yanes::vrc6_saw(phase, shape * 2 + 1)));
      assert(std::abs(yanes::fds_wave(phase, shape)) <= 1.0f);
      assert(std::abs(yanes::n163_wave(phase, shape)) <= 1.0f);
      assert(std::abs(yanes::vrc7_fm(phase, 2.0, 3.0)) <= 1.0f);
    }
  }

  uint32_t gb = 1;
  for (int i = 0; i < 1000; ++i) gb = yanes::game_boy_lfsr_clock(gb, false);
  // The register must keep moving from the value the hardware loads on trigger.
  // "Not zero and in range" is satisfied by a frozen register, so count output
  // transitions instead: an absorbing state scores zero and a real sequence
  // lands near half the clocks.
  for (bool narrow : {false, true}) {
    uint32_t state = 0x7fff;
    uint32_t previous = state & 1U;
    int transitions = 0;
    for (int i = 0; i < 4096; ++i) {
      state = yanes::game_boy_lfsr_clock(state, narrow);
      transitions += (state & 1U) != previous;
      previous = state & 1U;
    }
    assert(transitions > 1024);
  }
  const uint8_t gb_wave[16] = {0x0f, 0x18, 0x27, 0x36, 0x45, 0x54, 0x63, 0x72,
                               0x81, 0x90, 0xaf, 0xbe, 0xcd, 0xdc, 0xeb, 0xfa};
  assert(yanes::game_boy_wave_sample(0.0, gb_wave) == -1.0f);
  assert(yanes::game_boy_wave_sample(1.0 / 32.0, gb_wave) == 1.0f);
  assert(yanes::game_boy_wave_level(1.0f, 0) == 0.0f);
  assert(yanes::game_boy_wave_level(1.0f, 2) == 0.5f);
  assert(yanes::game_boy_wave_level(1.0f, 3) == 0.25f);
  uint8_t pce_wave[32]{};
  pce_wave[0] = 0;
  pce_wave[1] = 31;
  assert(yanes::pce_wave_sample(0.0, pce_wave) == -1.0f);
  assert(yanes::pce_wave_sample(1.0 / 32.0, pce_wave) == 1.0f);
  assert(gb != 0 && gb < 0x8000);
  uint32_t sms_white = 1, sms_periodic = 1;
  for (int i = 0; i < 100; ++i) {
    sms_white = yanes::sega_psg_lfsr_clock(sms_white, true);
    sms_periodic = yanes::sega_psg_lfsr_clock(sms_periodic, false);
  }
  assert(sms_white != sms_periodic);
  uint32_t pce = 1;
  for (int i = 0; i < 1000; ++i) pce = yanes::pce_lfsr_clock(pce);
  assert(pce != 0 && pce < 0x40000);

  for (int i = 0; i < 256; ++i) {
    const double phase = (i + 0.5) / 256.0;
    assert(std::abs(yanes::morph_wavetable(phase, 0.37, 0.61)) <= 1.0f);
    assert(std::abs(yanes::phase_distortion(phase, 0.8, 0.3)) <= 1.0f);
    assert(std::isfinite(yanes::additive(phase, 0.5, 0.4)));
    assert(std::abs(yanes::six_operator_fm(phase, 28, 4.0, 0.7)) <= 1.0f);
    assert(std::abs(yanes::porta_fm(phase, 3.0, 2.7, 0.6)) <= 1.0f);
    assert(std::abs(yanes::analog_poly(phase, std::fmod(phase * 1.0045, 1.0), 0.5)) <= 1.0f);
    assert(std::abs(yanes::digital_ensemble(phase, 0.6)) <= 1.0f);
    assert(std::abs(yanes::tine_piano(phase, 3.2, 0.65, 0.2)) <= 1.0f);
  }

  // --- SNES S-DSP core ---------------------------------------------------------
  // Each fractional position's four Gaussian weights sum to ~2048, so the >>11 in
  // the interpolator is unity gain and a constant signal passes through unchanged.
  for (int frac = 0; frac < 4096; frac += 17) {
    const int offset = (frac >> 4) & 0xFF;
    const int sum = yanes::snes::kGauss[static_cast<size_t>(255 - offset)] +
                    yanes::snes::kGauss[static_cast<size_t>(511 - offset)] +
                    yanes::snes::kGauss[static_cast<size_t>(256 + offset)] +
                    yanes::snes::kGauss[static_cast<size_t>(offset)];
    assert(std::abs(sum - 2048) <= 2);
    const int dc = yanes::snes::interpolate(1000, 1000, 1000, 1000, frac);
    assert(std::abs(dc - 1000) <= 12);  // per-tap flooring + cleared low bit
    // Interpolating a rising ramp lands between the two centre samples.
    const int ramp = yanes::snes::interpolate(0, 1000, 2000, 3000, frac);
    assert(ramp >= 800 && ramp <= 2200);
  }

  // BRR round-trips a smooth sine with low error (it is near-lossless there),
  // reports the whole-sample loop, and preserves length.
  {
    std::vector<int16_t> sine(256);
    for (size_t i = 0; i < sine.size(); ++i)
      sine[i] = static_cast<int16_t>(std::lround(12000.0 * std::sin(6.28318530718 * i / 64.0)));
    const auto brr = yanes::snes::brr_encode(sine.data(), sine.size(), true);
    assert(brr.size() == (sine.size() / 16) * 9);
    const auto decoded = yanes::snes::brr_decode(brr.data(), brr.size());
    assert(decoded.pcm.size() == sine.size());
    assert(decoded.loop == 0);  // whole-sample loop
    double energy = 0.0, error = 0.0;
    for (size_t i = 0; i < sine.size(); ++i) {
      energy += static_cast<double>(sine[i]) * sine[i];
      const double e = static_cast<double>(decoded.pcm[i]) - sine[i];
      error += e * e;
    }
    assert(error / energy < 0.05 && "BRR round-trip of a sine is near-lossless");
  }

  // A .brr file image's two-byte loop-offset header sets the loop point, instead
  // of always looping to the start.
  {
    std::vector<int16_t> tone(160);
    for (size_t i = 0; i < tone.size(); ++i)
      tone[i] = static_cast<int16_t>(std::lround(9000.0 * std::sin(6.28318530718 * i / 40.0)));
    const auto blocks = yanes::snes::brr_encode(tone.data(), tone.size(), true);
    std::vector<uint8_t> file;
    const uint16_t loop_offset = 5 * 9;  // loop back to block 5 -> sample 80
    file.push_back(static_cast<uint8_t>(loop_offset & 0xff));
    file.push_back(static_cast<uint8_t>(loop_offset >> 8));
    file.insert(file.end(), blocks.begin(), blocks.end());
    const auto decoded = yanes::snes::brr_decode_file(file.data(), file.size());
    assert(decoded.loop == 5 * 16 && "the .brr loop header sets a mid-sample loop point");
    // A bare block stream (no header) still loops to the start.
    assert(yanes::snes::brr_decode_file(blocks.data(), blocks.size()).loop == 0);
  }

  // A one-shot (non-looping) BRR sample decodes to a finite length and stops.
  {
    std::vector<int16_t> ramp(48);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<int16_t>(i * 200 - 4800);
    const auto brr = yanes::snes::brr_encode(ramp.data(), ramp.size(), false);
    const auto decoded = yanes::snes::brr_decode(brr.data(), brr.size());
    assert(decoded.loop == -1);
    assert(decoded.pcm.size() == ramp.size());
  }

  // The echo FIR returns finite output and decays once its input stops.
  {
    yanes::snes::Echo echo;
    echo.configure(480, {{64, 0, 0, 0, 0, 0, 0, 0}}, 0.4f, 0.6f);
    for (int i = 0; i < 48000; ++i) { const float out = echo.process(i < 240 ? 0.5f : 0.0f); assert(std::isfinite(out)); }
    assert(std::abs(echo.process(0.0f)) < 0.5f);
  }
}
