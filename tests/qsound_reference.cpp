// Isolated wrapper around superctr's Capcom DL-1425 QSound emulator (the core MAME and Furnace
// use, written from the DSP program). Compiled separately so its C headers cannot disturb the
// parity test's own translation unit. Test-only; never part of the product.
#include "qsound.h"

#include <cstdint>
#include <vector>

// Plays an 8-bit looping sample on PCM voice 0 at rate register `rate` (4.12 fixed point: 0x1000
// is one sample per output) and centre pan, through the chip's output stage: with `surround` the
// Q1 FIR and delays (the power-on pan, which Furnace keeps), without it the linear pan table.
// Returns `frames` stereo samples at the chip's own 24038 Hz rate, scaled to [-1, 1].
void qsound_reference_render(const std::vector<int8_t>& loop, uint16_t rate, bool surround, int frames,
                             std::vector<float>& left, std::vector<float>& right) {
  static qsound_chip chip;
  qsound_start(&chip, 60000000);
  std::vector<uint8_t> rom(0x10000);
  for (size_t i = 0; i < loop.size(); ++i) rom[i] = static_cast<uint8_t>(loop[i]);
  chip.rom_data = rom.data();
  chip.rom_mask = 0xffff;
  // Let the DSP program run its init and filter refresh before writing any voice.
  for (int i = 0; i < 64; ++i) qsound_update(&chip);
  const auto length = static_cast<uint16_t>(loop.size());
  qsound_write_data(&chip, 0x01, 0);       // voice 0 start address
  qsound_write_data(&chip, 0x03, 0);       // phase
  qsound_write_data(&chip, 0x04, length);  // loop length
  qsound_write_data(&chip, 0x05, length);  // end address
  qsound_write_data(&chip, 0x80, surround ? 0x120 : 0x150);  // centre, Q1 or linear table
  qsound_write_data(&chip, 0x06, 0x2000);  // volume
  qsound_write_data(&chip, 0x02, rate);    // starts the voice moving
  left.assign(static_cast<size_t>(frames), 0.0f);
  right.assign(static_cast<size_t>(frames), 0.0f);
  for (int i = 0; i < frames; ++i) {
    qsound_update(&chip);
    left[static_cast<size_t>(i)] = chip.out[0] / 32768.0f;
    right[static_cast<size_t>(i)] = chip.out[1] / 32768.0f;
  }
}
