// Game Boy hardware oracle: runs a real ROM under SameBoy and emits both halves
// of the gate from the same run — the APU register writes the game's sound
// driver issued, and the reference audio SameBoy produced from them.
//
// SameBoy is the accuracy reference for DMG/CGB audio, and it is also the core
// Furnace uses for its Game Boy channel, so a YANES render scored against this
// oracle is scored against the same ceiling Furnace sits at.
//
// Per-channel isolation: GB_set_channel_muted lets the oracle render each of the
// four channels alone. The gate then reports which voice diverged instead of
// only that the mix did.
//
// Requires the SameBoy APU register hook (third_party/sameboy-apu-register-log.patch).

#include <Core/gb.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000

static int16_t *g_pcm;
static size_t g_pcm_len, g_pcm_cap;
static uint64_t g_sample_index;
static FILE *g_reg_log;

static void push_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
  (void)gb;
  if (g_pcm_len + 2 > g_pcm_cap) {
    g_pcm_cap = g_pcm_cap ? g_pcm_cap * 2 : 1 << 20;
    g_pcm = realloc(g_pcm, g_pcm_cap * sizeof(int16_t));
    if (!g_pcm) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  g_pcm[g_pcm_len++] = sample->left;
  g_pcm[g_pcm_len++] = sample->right;
  ++g_sample_index;
}

// Raw bus write, before NR52 gating and before the DMG wave-RAM redirect: this
// is what the sound driver asked for, which is what YANES must reproduce.
static void log_register(GB_gameboy_t *gb, uint8_t reg, uint8_t value) {
  (void)gb;
  if (g_reg_log) {
    fprintf(g_reg_log, "%llu %02x %02x\n",
            (unsigned long long)g_sample_index, reg, value);
  }
}

// A Game Boy powers on at a title screen and holds its sound driver there, so a
// capture with no input is a capture of the title jingle at best and silence at
// worst. The oracle presses Start on a fixed schedule to reach the music the
// game actually plays; the schedule is frame numbers, so a run stays
// reproducible. YANES_AUTO_START_FRAMES overrides it for a title that needs a
// different path in (a name-entry screen, a longer intro).
#define GB_FRAME_RATE 59.7275
static bool auto_start_held(long frame) {
  const char *list = getenv("YANES_AUTO_START_FRAMES");
  if (!list || !*list) list = "60,150,240,330";
  while (*list) {
    char *end = NULL;
    const long at = strtol(list, &end, 10);
    if (end == list) break;
    if (frame >= at && frame < at + 8) return true;
    list = (*end == ',') ? end + 1 : end;
  }
  return false;
}

static const char *g_boot_path;
static void load_boot(GB_gameboy_t *gb, GB_boot_rom_t type) {
  (void)type;
  if (GB_load_boot_rom(gb, g_boot_path)) {
    fprintf(stderr, "warning: could not load boot ROM %s\n", g_boot_path);
  }
}

// SameBoy renders video unconditionally, so even an audio-only oracle has to
// give it somewhere to put pixels and a way to pack them.
static uint32_t g_screen[256 * 256];
static uint32_t encode_rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
  (void)gb;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void write_wav(const char *path, const int16_t *pcm, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  const uint32_t data_bytes = (uint32_t)(len * sizeof(int16_t));
  const uint32_t file_bytes = 36 + data_bytes;
  const uint32_t fmt_len = 16, rate = SAMPLE_RATE;
  const uint16_t fmt_type = 1, channels = 2, bits = 16, align = 4;
  const uint32_t byte_rate = rate * align;
  fwrite("RIFF", 1, 4, f); fwrite(&file_bytes, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_len, 4, 1, f); fwrite(&fmt_type, 2, 1, f); fwrite(&channels, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
  fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
  fwrite(pcm, 1, data_bytes, f);
  fclose(f);
}

// One deterministic run from power-on. `solo` < 0 renders the full mix and logs
// registers; otherwise only that channel sounds and no log is written.
static void run_pass(const char *rom, GB_model_t model, double seconds,
                     int solo, const char *wav_path, const char *reg_path) {
  // GB_gameboy_t is far too large for the stack; SameBoy's own header documents
  // the GB_alloc form for exactly this reason.
  GB_gameboy_t *gb = GB_init(GB_alloc(), model);
  GB_set_boot_rom_load_callback(gb, load_boot);
  if (GB_load_rom(gb, rom)) { fprintf(stderr, "cannot load ROM %s\n", rom); exit(1); }
  GB_set_rgb_encode_callback(gb, encode_rgb);
  GB_set_pixels_output(gb, g_screen);
  GB_set_sample_rate(gb, SAMPLE_RATE);
  GB_apu_set_sample_callback(gb, push_sample);
  GB_reset(gb);

  for (int ch = 0; ch < GB_N_CHANNELS; ++ch) {
    GB_set_channel_muted(gb, (GB_channel_t)ch, solo >= 0 && ch != solo);
  }

  g_pcm_len = 0; g_sample_index = 0;
  g_reg_log = reg_path ? fopen(reg_path, "w") : NULL;
  if (g_reg_log) {
    fprintf(g_reg_log, "# SameBoy APU register writes from %s\n", rom);
    fprintf(g_reg_log, "# columns: sample_index_at_48000Hz  reg_offset_from_FF10  value\n");
  }
  GB_apu_register_callback = g_reg_log ? log_register : NULL;

  const uint64_t want = (uint64_t)(seconds * SAMPLE_RATE);
  bool start_down = false;
  while (g_sample_index < want) {
    const long frame =
        (long)((double)g_sample_index * GB_FRAME_RATE / SAMPLE_RATE);
    const bool press = auto_start_held(frame);
    if (press != start_down) {
      GB_set_key_state(gb, GB_KEY_START, press);
      start_down = press;
    }
    GB_run(gb);
  }

  GB_apu_register_callback = NULL;
  if (g_reg_log) { fclose(g_reg_log); g_reg_log = NULL; }
  write_wav(wav_path, g_pcm, g_pcm_len);
  GB_free(gb);
  free(gb);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: yanes-gb-oracle <rom.gb> <out_dir> [seconds] [boot_rom.bin]\n"
            "  Writes <out_dir>/gb_registers.log plus gb_mix.wav and gb_ch1..4.wav\n");
    return 2;
  }
  const char *rom = argv[1];
  const char *out_dir = argv[2];
  const double seconds = argc >= 4 ? atof(argv[3]) : 5.0;
  static char boot_buf[1024];
  if (argc >= 5) {
    g_boot_path = argv[4];
  } else {
    const char *home = getenv("HOME");
    snprintf(boot_buf, sizeof boot_buf, "%s/.config/retroarch/system/dmg_boot.bin",
             home ? home : ".");
    g_boot_path = boot_buf;
  }

  // .gbc content wants a Color boot ROM; plain .gb is graded on DMG hardware.
  const size_t len = strlen(rom);
  const bool color = len > 4 && strcmp(rom + len - 4, ".gbc") == 0;
  const GB_model_t model = color ? GB_MODEL_CGB_E : GB_MODEL_DMG_B;
  static char cgb_buf[1024];
  if (color && argc < 5) {
    const char *home = getenv("HOME");
    snprintf(cgb_buf, sizeof cgb_buf, "%s/.config/retroarch/system/cgb_boot.bin",
             home ? home : ".");
    g_boot_path = cgb_buf;
  }

  char path[1024], reg[1024];
  snprintf(reg, sizeof reg, "%s/gb_registers.log", out_dir);
  snprintf(path, sizeof path, "%s/gb_mix.wav", out_dir);
  run_pass(rom, model, seconds, -1, path, reg);
  printf("oracle: %s (%s) %.2fs -> %s\n", rom, color ? "CGB" : "DMG", seconds, out_dir);

  static const char *names[GB_N_CHANNELS] = {"ch1_pulse", "ch2_pulse", "ch3_wave", "ch4_noise"};
  for (int ch = 0; ch < GB_N_CHANNELS; ++ch) {
    snprintf(path, sizeof path, "%s/gb_%s.wav", out_dir, names[ch]);
    run_pass(rom, model, seconds, ch, path, NULL);
  }
  return 0;
}
