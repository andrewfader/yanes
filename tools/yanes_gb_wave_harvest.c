// Game Boy wave-table harvest: runs a real ROM under SameBoy for a long single
// pass with a fixed, seeded input schedule and logs every APU register write.
// tools/gb_wave_tables.py turns the log into the distinct CH3 wave-RAM tables the
// game's sound driver actually played, weighted by how long each one sounded.
// This is the research side of the Game Boy wave presets; the presets' own
// parity lane (tools/test_gb_wave_preset_parity.sh) does not need a ROM.
//
// The oracle (tools/yanes_gb_oracle.c) only taps Start, which holds most games at
// a title screen. Here A is tapped twice a second to page through dialogue and
// menus, Start every seven seconds, and a seeded walk on the d-pad moves around
// once a game is in play, so a run reaches the in-game songs too.
//
// Requires the SameBoy APU register hook (third_party/sameboy-apu-register-log.patch).

#include <Core/gb.h>
#include <Core/random.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define GB_FRAME_RATE 59.7275

static uint64_t g_sample_index;
static FILE *g_reg_log;

static void push_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
  (void)gb; (void)sample;
  ++g_sample_index;
}

static void log_register(GB_gameboy_t *gb, uint8_t reg, uint8_t value) {
  (void)gb;
  fprintf(g_reg_log, "%llu %02x %02x\n", (unsigned long long)g_sample_index, reg, value);
}

static const char *g_boot_path;
static void load_boot(GB_gameboy_t *gb, GB_boot_rom_t type) {
  (void)type;
  if (GB_load_boot_rom(gb, g_boot_path)) fprintf(stderr, "warning: could not load boot ROM %s\n", g_boot_path);
}

static uint32_t g_screen[256 * 256];
static uint32_t encode_rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
  (void)gb;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Which keys are down on `frame`. Deterministic: the d-pad walk is an LCG on the
// half-second index, so a run is reproducible.
static void keys_for_frame(long frame, bool keys[GB_KEY_MAX]) {
  memset(keys, 0, sizeof(bool) * GB_KEY_MAX);
  if (frame % 30 < 4) keys[GB_KEY_A] = true;
  if (frame % 418 >= 200 && frame % 418 < 206) keys[GB_KEY_START] = true;
  const long slot = frame / 30;
  uint32_t r = (uint32_t)slot * 1103515245u + 12345u;
  r = (r >> 16) & 7;
  static const GB_key_t pad[4] = {GB_KEY_RIGHT, GB_KEY_LEFT, GB_KEY_UP, GB_KEY_DOWN};
  if (r < 4 && frame % 30 >= 8) keys[pad[r]] = true;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: yanes-gb-wave-harvest <rom.gb> <registers.log> [seconds]\n");
    return 2;
  }
  const char *rom = argv[1];
  const double seconds = argc >= 4 ? atof(argv[3]) : 600.0;
  const size_t len = strlen(rom);
  const bool color = len > 4 && strcmp(rom + len - 4, ".gbc") == 0;
  static char boot_buf[1024];
  const char *home = getenv("HOME");
  snprintf(boot_buf, sizeof boot_buf, "%s/.config/retroarch/system/%s", home ? home : ".",
           color ? "cgb_boot.bin" : "dmg_boot.bin");
  g_boot_path = boot_buf;

  g_reg_log = fopen(argv[2], "w");
  if (!g_reg_log) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
  fprintf(g_reg_log, "# SameBoy APU register writes from %s\n", rom);
  fprintf(g_reg_log, "# columns: sample_index_at_48000Hz  reg_offset_from_FF10  value\n");

  GB_random_seed(0);
  GB_gameboy_t *gb = GB_init(GB_alloc(), color ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
  GB_set_boot_rom_load_callback(gb, load_boot);
  if (GB_load_rom(gb, rom)) { fprintf(stderr, "cannot load ROM %s\n", rom); return 1; }
  GB_set_rgb_encode_callback(gb, encode_rgb);
  GB_set_pixels_output(gb, g_screen);
  GB_set_sample_rate(gb, SAMPLE_RATE);
  GB_apu_set_sample_callback(gb, push_sample);
  GB_reset(gb);
  GB_set_turbo_mode(gb, true, true);
  GB_apu_register_callback = log_register;

  const uint64_t want = (uint64_t)(seconds * SAMPLE_RATE);
  bool down[GB_KEY_MAX] = {0}, keys[GB_KEY_MAX];
  while (g_sample_index < want) {
    keys_for_frame((long)((double)g_sample_index * GB_FRAME_RATE / SAMPLE_RATE), keys);
    for (int k = 0; k < GB_KEY_MAX; ++k) {
      if (keys[k] != down[k]) { GB_set_key_state(gb, (GB_key_t)k, keys[k]); down[k] = keys[k]; }
    }
    GB_run(gb);
  }
  GB_apu_register_callback = NULL;
  fclose(g_reg_log);
  GB_free(gb);
  free(gb);
  printf("harvest: %s (%s) %.0fs -> %s\n", rom, color ? "CGB" : "DMG", seconds, argv[2]);
  return 0;
}
