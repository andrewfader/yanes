#include "../src/dsp.hpp"
#include <cassert>
#include <cmath>
#include <set>

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
}
