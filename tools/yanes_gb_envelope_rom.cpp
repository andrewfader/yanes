// Writes a small Game Boy ROM whose only job is to exercise the APU envelopes, so
// the SameBoy lane has a stimulus that is ours to ship (no commercial ROM, no
// Nintendo boot ROM: tools/yanes_gb_oracle.c boots it with a stub).
//
// The ROM is a register-script interpreter in hand-assembled SM83 code. The
// script plays, one channel at a time: pulse decay, attack then retrigger-to-
// release, the length counter, a downward sweep, a slow 12.5% attack cut by the
// DAC switching off, the wave channel's volume steps, and the noise envelope in
// both LFSR widths. Each segment is something YANES's replay has to get right
// on its own (frame sequencer, envelope, length, sweep, DAC), scored against
// what SameBoy does with the same register writes.
//
//   yanes-gb-envelope-rom out.gb

#include "gb_script_rom.hpp"

#include <array>
#include <cstdio>

namespace {

using namespace gb_script_rom;

std::vector<uint8_t> script() {
  Script s;
  auto w = [&](uint8_t reg, uint8_t value) { s.write(reg, value); };
  auto wait = [&](int tens_of_ms) { s.wait(tens_of_ms); };
  w(NR52, 0x80); w(NR51, 0xff); w(NR50, 0x77);
  wait(10);

  // CH1 at ~440 Hz (period 1750).
  w(NR10, 0x00); w(NR11, 0x80); w(NR12, 0xf3); w(NR13, 0xd6); w(NR14, 0x86);  // decay, pace 3
  wait(100);
  w(NR12, 0x0a); w(NR14, 0x86);  // attack from 0, pace 2
  wait(80);
  w(NR12, 0xf2); w(NR14, 0x86);  // retrigger at full into a pace-2 release
  wait(70);
  w(NR11, 0xa0); w(NR12, 0xf0); w(NR14, 0xc6);  // held level, 125 ms length counter
  wait(40);
  w(NR10, 0x2b); w(NR12, 0xf1); w(NR14, 0x86);  // downward sweep under a fast decay
  wait(60);
  w(NR10, 0x00);

  // CH2 at ~290 Hz, 12.5% duty.
  w(NR21, 0x00); w(NR22, 0xf1); w(NR23, 0x3b); w(NR24, 0x86);  // fast decay
  wait(50);
  w(NR22, 0x1f); w(NR24, 0x86);  // slow attack from 1, pace 7
  wait(120);
  w(NR22, 0x00);  // DAC off mid-note: a hard cut
  wait(30);

  // CH3 at ~220 Hz on a triangle table, stepping through the volume codes.
  w(NR30, 0x00);
  constexpr std::array<uint8_t, 16> triangle{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                             0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  for (size_t i = 0; i < triangle.size(); ++i) w(static_cast<uint8_t>(WAVE + i), triangle[i]);
  w(NR30, 0x80); w(NR31, 0x00); w(NR32, 0x20); w(NR33, 0xd6); w(NR34, 0x86);
  wait(40);
  w(NR32, 0x40); wait(40);  // 50%
  w(NR32, 0x60); wait(40);  // 25%
  w(NR32, 0x00); wait(30);  // mute
  w(NR30, 0x00); wait(20);

  // CH4: 15-bit LFSR decay, 7-bit LFSR attack, then a retriggered release.
  w(NR41, 0x00); w(NR42, 0xf2); w(NR43, 0x44); w(NR44, 0x80);
  wait(70);
  w(NR42, 0x0b); w(NR43, 0x3c); w(NR44, 0x80);
  wait(100);
  w(NR42, 0xf1); w(NR44, 0x80);
  wait(40);
  return s.finish();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: yanes-gb-envelope-rom out.gb\n");
    return 2;
  }
  return write_rom(argv[1], "YANES ENV", script()) ? 0 : 1;
}
