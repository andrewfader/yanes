// Isolated wrapper around the reference S-DSP (blargg's SPC_DSP). It is compiled
// separately so the blargg headers' NDEBUG define stays out of the parity test's
// translation unit (which needs its assertions live). Links only when the
// YANES_SNES_DSP_REFERENCE sources are supplied; never part of the product.
#include "SPC_DSP.h"

#include <cstdint>
#include <cstring>
#include <vector>

// One mono voice from the reference S-DSP: a single BRR sample on voice 0, no
// envelope (constant direct GAIN), no echo, played at `pitch` (0x1000 == native).
std::vector<float> snes_reference_render(const uint8_t* blocks, int block_bytes, int loop_block, int pitch, int frames) {
  static uint8_t ram[0x10000];
  std::memset(ram, 0, sizeof(ram));
  const int dir_page = 0x02, data = 0x0300;
  for (int i = 0; i < block_bytes; ++i) ram[(data + i) & 0xffff] = blocks[i];
  const int loop_addr = data + (loop_block > 0 ? loop_block : 0) * 9;
  ram[0x0200] = data & 0xff;      ram[0x0201] = (data >> 8) & 0xff;
  ram[0x0202] = loop_addr & 0xff; ram[0x0203] = (loop_addr >> 8) & 0xff;

  SPC_DSP dsp;
  dsp.init(ram);
  dsp.soft_reset();
  dsp.write(0x6C, 0x20);  // FLG: reset off, not muted, echo-buffer writes disabled
  dsp.write(0x7D, 0x00);  // echo delay 0
  dsp.write(0x4D, 0x00);  // echo off
  dsp.write(0x3D, 0x00);  // no noise
  dsp.write(0x2D, 0x00);  // no pitch modulation
  dsp.write(0x5D, dir_page);
  dsp.write(0x0C, 0x7F); dsp.write(0x1C, 0x7F);  // master volume L/R
  dsp.write(0x00, 0x7F); dsp.write(0x01, 0x7F);  // voice 0 volume L/R
  dsp.write(0x02, pitch & 0xff); dsp.write(0x03, (pitch >> 8) & 0x3f);
  dsp.write(0x04, 0x00);  // source 0
  dsp.write(0x05, 0x00);  // ADSR disabled -> GAIN mode
  dsp.write(0x07, 0x7f);  // direct GAIN, full
  dsp.write(0x4C, 0x01);  // key on voice 0

  std::vector<float> out;
  out.reserve(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    SPC_DSP::sample_t s[2] = {0, 0};
    dsp.set_output(s, 1);
    dsp.run(32);  // 32 master clocks == one output sample
    out.push_back(s[0] / 32768.0f);
  }
  return out;
}
