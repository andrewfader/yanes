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
  }
}
