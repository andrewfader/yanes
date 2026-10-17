// A Game Boy ROM that is nothing but an APU register-script interpreter, for the
// SameBoy lanes: tools/yanes_gb_oracle.c boots it with a stub boot ROM, so a lane
// needs no commercial ROM and no Nintendo boot ROM.
//
// A script is a byte string of (register, value) pairs written with
// `ld ($FF00+c),a`, `kWait n` to wait n x 10 ms, and `kEnd` to park the CPU.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace gb_script_rom {

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

class Script {
 public:
  void write(uint8_t reg, uint8_t value) { bytes_.push_back(reg); bytes_.push_back(value); }
  void wait(int tens_of_ms) {
    while (tens_of_ms > 0) {
      const int n = tens_of_ms > 255 ? 255 : tens_of_ms;
      bytes_.push_back(kWait); bytes_.push_back(static_cast<uint8_t>(n));
      tens_of_ms -= n;
    }
  }
  std::vector<uint8_t> finish() const { auto s = bytes_; s.push_back(kEnd); return s; }

 private:
  std::vector<uint8_t> bytes_;
};

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

// Assembles the interpreter, appends the script and fixes up the header checksums.
inline bool write_rom(const char* path, const char* title, const std::vector<uint8_t>& s) {
  std::vector<uint8_t> rom(0x8000, 0x00);
  constexpr uint16_t kCode = 0x0150, kScript = 0x0200;
  // Entry point: nop; jp $0150.
  rom[0x100] = 0x00; rom[0x101] = 0xc3; rom[0x102] = kCode & 0xff; rom[0x103] = kCode >> 8;
  for (size_t i = 0; i < 15 && title[i]; ++i) rom[0x134 + i] = static_cast<uint8_t>(title[i]);
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
  if (a.pc > kScript) { std::fprintf(stderr, "interpreter overruns the script\n"); return false; }

  if (kScript + s.size() > rom.size()) { std::fprintf(stderr, "script too long\n"); return false; }
  for (size_t i = 0; i < s.size(); ++i) rom[kScript + i] = s[i];

  uint8_t header = 0;
  for (size_t i = 0x134; i <= 0x14c; ++i) header = static_cast<uint8_t>(header - rom[i] - 1);
  rom[0x14d] = header;
  uint16_t global = 0;
  for (size_t i = 0; i < rom.size(); ++i)
    if (i != 0x14e && i != 0x14f) global = static_cast<uint16_t>(global + rom[i]);
  rom[0x14e] = static_cast<uint8_t>(global >> 8);
  rom[0x14f] = static_cast<uint8_t>(global);

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
  return static_cast<bool>(out);
}

}  // namespace gb_script_rom
