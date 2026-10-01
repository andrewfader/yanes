// Isolated wrapper around reSIDfp, the reference MOS6581/8580 emulation (the same
// core the accurate SID players use). Compiled separately so the library's
// headers cannot disturb the parity test's own translation unit. Linked only when
// libresidfp is present; never part of the product.
#include "residfp.h"

#include <cmath>
#include <vector>

// Render one SID voice from reSIDfp: waveform `wave` (SID control-register bits)
// at `hz`, pulse width `pw` (0..1), with an instant full-level envelope and the
// filter bypassed, on the given chip (0 = 6581, 1 = 8580), at 44100 Hz.
std::vector<float> sid_reference_render(int chip, int wave, double hz, double pw, int frames) {
  constexpr double kClock = 1022727.0;  // NTSC C64
  reSIDfp::residfp sid;
  sid.setChipModel(chip == 0 ? reSIDfp::MOS6581 : reSIDfp::CSG8580);
  sid.setSamplingParameters(kClock, reSIDfp::DECIMATE, 44100.0);
  sid.reset();
  const int fn = static_cast<int>(std::lround(hz * 16777216.0 / kClock));
  sid.write(0, fn & 0xff);
  sid.write(1, (fn >> 8) & 0xff);
  const int pwi = static_cast<int>(std::lround(pw * 4095.0));
  sid.write(2, pwi & 0xff);
  sid.write(3, (pwi >> 8) & 0x0f);
  sid.write(5, 0x00);  // attack 0, decay 0
  sid.write(6, 0xf0);  // sustain 15, release 0
  sid.write(24, 0x0f); // master volume max, filter off
  sid.write(4, (wave | 0x01) & 0xff);  // waveform select + gate on

  std::vector<float> out;
  out.reserve(static_cast<size_t>(frames));
  short buf[2048];
  while (static_cast<int>(out.size()) < frames) {
    sid.clock(buf, 2048);
    for (int i = 0; i < 2048 && static_cast<int>(out.size()) < frames; ++i)
      out.push_back(buf[i] / 32768.0f);
  }
  return out;
}
