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

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

constexpr uint8_t kWait = 0x00;  // script: 0x00 n  -> wait n x 10 ms
constexpr uint8_t kEnd = 0xff;   // script: 0xff    -> stop

// APU registers by their offset from 0xFF00, which is what `ld ($FF00+c),a` takes.
enum : uint8_t {
  NR10 = 0x10, NR11, NR12, NR13, NR14,
  NR21 = 0x16, NR22, NR23, NR24,
  NR30 = 0x1a, NR31, NR32, NR33, NR34,
  NR41 = 0x20, NR42, NR43, NR44,
  NR50 = 0x24, NR51, NR52,
  WAVE = 0x30,
};

std::vector<uint8_t> script() {
  std::vector<uint8_t> s;
  auto w = [&](uint8_t reg, uint8_t value) { s.push_back(reg); s.push_back(value); };
  auto wait = [&](int tens_of_ms) {
    while (tens_of_ms > 0) {
      const int n = tens_of_ms > 255 ? 255 : tens_of_ms;
      s.push_back(kWait); s.push_back(static_cast<uint8_t>(n));
      tens_of_ms -= n;
    }
  };
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
  s.push_back(kEnd);
  return s;
}

// Minimal label-resolving assembler for the interpreter loop.
struct Asm {
  std::vector<uint8_t>& rom;
  uint16_t pc;
  void byte(uint8_t b) { rom[pc++] = b; }
  void word(uint16_t v) { byte(static_cast<uint8_t>(v)); byte(static_cast<uint8_t>(v >> 8)); }
  // Relative jump to `target`; the offset is from the byte after the operand.
  void jr(uint8_t opcode, uint16_t target) {
    byte(opcode);
    byte(static_cast<uint8_t>(static_cast<int>(target) - static_cast<int>(pc + 1)));
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: yanes-gb-envelope-rom out.gb\n");
    return 2;
  }
  std::vector<uint8_t> rom(0x8000, 0x00);
  constexpr uint16_t kCode = 0x0150, kScript = 0x0200;
  // Entry point: nop; jp $0150.
  rom[0x100] = 0x00; rom[0x101] = 0xc3; rom[0x102] = kCode & 0xff; rom[0x103] = kCode >> 8;
  const char title[] = "YANES ENV";
  for (size_t i = 0; i + 1 < sizeof title; ++i) rom[0x134 + i] = static_cast<uint8_t>(title[i]);
  rom[0x147] = 0x00;  // ROM only
  rom[0x148] = 0x00;  // 32 KiB
  rom[0x14a] = 0x01;  // non-Japanese

  // One wait unit is 10 ms: the inner loop (dec de; ld a,d; or e; jr nz) is
  // 7 M-cycles, 1498 of them at 1.048576 MHz.
  constexpr uint16_t kInnerCount = 1498;
  Asm a{rom, kCode};
  a.byte(0xf3);                            // di
  a.byte(0x31); a.word(0xfffe);            // ld sp,$fffe
  a.byte(0x21); a.word(kScript);           // ld hl,script
  const uint16_t loop = a.pc;
  a.byte(0x2a);                            // ld a,(hl+)
  a.byte(0xb7);                            // or a
  const uint16_t jr_wait = a.pc; a.pc += 2;   // jr z,wait (patched)
  a.byte(0xfe); a.byte(kEnd);              // cp $ff
  const uint16_t jr_end = a.pc; a.pc += 2;    // jr z,end (patched)
  a.byte(0x4f);                            // ld c,a
  a.byte(0x2a);                            // ld a,(hl+)
  a.byte(0xe2);                            // ld ($ff00+c),a
  a.jr(0x18, loop);                        // jr loop
  const uint16_t wait = a.pc;
  a.byte(0x46);                            // ld b,(hl)
  a.byte(0x23);                            // inc hl
  const uint16_t outer = a.pc;
  a.byte(0x11); a.word(kInnerCount);       // ld de,count
  const uint16_t inner = a.pc;
  a.byte(0x1b);                            // dec de
  a.byte(0x7a);                            // ld a,d
  a.byte(0xb3);                            // or e
  a.jr(0x20, inner);                       // jr nz,inner
  a.byte(0x05);                            // dec b
  a.jr(0x20, outer);                       // jr nz,outer
  a.jr(0x18, loop);                        // jr loop
  const uint16_t end = a.pc;
  a.jr(0x18, end);                         // jr end (park)
  Asm{rom, jr_wait}.jr(0x28, wait);
  Asm{rom, jr_end}.jr(0x28, end);
  if (a.pc > kScript) { std::fprintf(stderr, "interpreter overruns the script\n"); return 1; }

  const std::vector<uint8_t> s = script();
  if (kScript + s.size() > rom.size()) { std::fprintf(stderr, "script too long\n"); return 1; }
  for (size_t i = 0; i < s.size(); ++i) rom[kScript + i] = s[i];

  uint8_t header = 0;
  for (size_t i = 0x134; i <= 0x14c; ++i) header = static_cast<uint8_t>(header - rom[i] - 1);
  rom[0x14d] = header;
  uint16_t global = 0;
  for (size_t i = 0; i < rom.size(); ++i)
    if (i != 0x14e && i != 0x14f) global = static_cast<uint16_t>(global + rom[i]);
  rom[0x14e] = static_cast<uint8_t>(global >> 8);
  rom[0x14f] = static_cast<uint8_t>(global);

  std::ofstream out(argv[1], std::ios::binary);
  out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
  return out ? 0 : 1;
}
