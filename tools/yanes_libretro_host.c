// Minimal libretro frontend for hardware oracle capture.
//
// Runs a ROM under a libretro core for a fixed number of frames and writes the
// core's audio to a WAV. Used with cores patched to log sound-chip register
// writes (see third_party/*.patch): one run yields both the register stream and
// the reference audio those registers produced, sample-aligned.
//
// Deliberately headless, and the only input it supplies is a scripted Start
// press on fixed frames, so a run is deterministic and a fixture is
// reproducible.

#include <libretro.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *g_pcm;
static size_t g_pcm_len, g_pcm_cap;
static double g_sample_rate = 44100.0;
static char g_system_dir[1024];
static long g_frame_index;

static void push(int16_t left, int16_t right) {
  if (g_pcm_len + 2 > g_pcm_cap) {
    g_pcm_cap = g_pcm_cap ? g_pcm_cap * 2 : 1 << 20;
    g_pcm = realloc(g_pcm, g_pcm_cap * sizeof(int16_t));
    if (!g_pcm) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  g_pcm[g_pcm_len++] = left;
  g_pcm[g_pcm_len++] = right;
}

static void audio_sample(int16_t left, int16_t right) { push(left, right); }

static size_t audio_batch(const int16_t *data, size_t frames) {
  for (size_t i = 0; i < frames; ++i) push(data[i * 2], data[i * 2 + 1]);
  return frames;
}

static void video_refresh(const void *d, unsigned w, unsigned h, size_t p) {
  (void)d; (void)w; (void)h; (void)p;
}
static void input_poll(void) {}

// Consoles boot into a title screen that holds the sound driver until Start is
// pressed, so a headless capture of a commercial ROM is silence unless the
// frontend supplies that press. YANES_AUTO_START_FRAMES lists the frames to hold
// Start on (six frames each, long enough for any poll rate); the default gets
// past a title screen and one following menu.
static int auto_start_held(long frame) {
  const char *list = getenv("YANES_AUTO_START_FRAMES");
  if (!list || !*list) list = "60,240";
  while (*list) {
    char *end = NULL;
    const long at = strtol(list, &end, 10);
    if (end == list) break;
    if (frame >= at && frame < at + 6) return 1;
    list = (*end == ',') ? end + 1 : end;
  }
  return 0;
}

static int16_t input_state(unsigned a, unsigned b, unsigned c, unsigned d) {
  (void)c;
  const char *auto_start = getenv("YANES_AUTO_START");
  if (auto_start && *auto_start && a == 0 && b == RETRO_DEVICE_JOYPAD &&
      d == RETRO_DEVICE_ID_JOYPAD_START && auto_start_held(g_frame_index))
    return 1;
  return 0;
}
static void core_log(enum retro_log_level level, const char *fmt, ...) {
  (void)level; (void)fmt;
}

static bool environment(unsigned cmd, void *data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = g_system_dir;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = core_log;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
      ((struct retro_variable *)data)->value = NULL;
      return false;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(bool *)data = true;
      return true;
    default:
      return false;
  }
}

static void write_wav(const char *path, const int16_t *pcm, size_t len, uint32_t rate) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  const uint32_t data_bytes = (uint32_t)(len * sizeof(int16_t));
  const uint32_t file_bytes = 36 + data_bytes, fmt_len = 16;
  const uint16_t fmt_type = 1, channels = 2, bits = 16, align = 4;
  const uint32_t byte_rate = rate * align;
  fwrite("RIFF", 1, 4, f); fwrite(&file_bytes, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt_len, 4, 1, f); fwrite(&fmt_type, 2, 1, f); fwrite(&channels, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
  fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
  fwrite(pcm, 1, data_bytes, f);
  fclose(f);
}

#define SYM(name) \
  do { \
    *(void **)(&name##_f) = dlsym(core, #name); \
    if (!name##_f) { fprintf(stderr, "core is missing %s\n", #name); return 1; } \
  } while (0)

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr,
            "usage: yanes-libretro-host <core.so> <rom> <out.wav> [seconds] [system_dir]\n"
            "  YANES_PSG_LOG (patched Beetle PCE) / YANES_APU_LOG (patched Nestopia)\n"
            "  also capture the chip's register stream.\n"
            "  YANES_AUTO_START=1 presses Start on YANES_AUTO_START_FRAMES (default 60,240).\n");
    return 2;
  }
  const char *core_path = argv[1], *rom = argv[2], *wav = argv[3];
  const double seconds = argc >= 5 ? atof(argv[4]) : 5.0;
  if (argc >= 6) {
    snprintf(g_system_dir, sizeof g_system_dir, "%s", argv[5]);
  } else {
    const char *home = getenv("HOME");
    snprintf(g_system_dir, sizeof g_system_dir, "%s/.config/retroarch/system", home ? home : ".");
  }

  void *core = dlopen(core_path, RTLD_LAZY);
  if (!core) { fprintf(stderr, "cannot load core: %s\n", dlerror()); return 1; }

  void (*retro_set_environment_f)(retro_environment_t);
  void (*retro_set_video_refresh_f)(retro_video_refresh_t);
  void (*retro_set_audio_sample_f)(retro_audio_sample_t);
  void (*retro_set_audio_sample_batch_f)(retro_audio_sample_batch_t);
  void (*retro_set_input_poll_f)(retro_input_poll_t);
  void (*retro_set_input_state_f)(retro_input_state_t);
  void (*retro_init_f)(void);
  void (*retro_deinit_f)(void);
  bool (*retro_load_game_f)(const struct retro_game_info *);
  void (*retro_unload_game_f)(void);
  void (*retro_get_system_av_info_f)(struct retro_system_av_info *);
  void (*retro_run_f)(void);

  SYM(retro_set_environment); SYM(retro_set_video_refresh);
  SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
  SYM(retro_set_input_poll); SYM(retro_set_input_state);
  SYM(retro_init); SYM(retro_deinit); SYM(retro_load_game);
  SYM(retro_unload_game); SYM(retro_get_system_av_info); SYM(retro_run);

  retro_set_environment_f(environment);
  retro_set_video_refresh_f(video_refresh);
  retro_set_audio_sample_f(audio_sample);
  retro_set_audio_sample_batch_f(audio_batch);
  retro_set_input_poll_f(input_poll);
  retro_set_input_state_f(input_state);
  retro_init_f();

  FILE *rf = fopen(rom, "rb");
  if (!rf) { fprintf(stderr, "cannot open ROM %s\n", rom); return 1; }
  fseek(rf, 0, SEEK_END);
  const long size = ftell(rf);
  fseek(rf, 0, SEEK_SET);
  void *data = malloc((size_t)size);
  if (fread(data, 1, (size_t)size, rf) != (size_t)size) {
    fprintf(stderr, "short read on %s\n", rom); return 1;
  }
  fclose(rf);

  struct retro_game_info info = {rom, data, (size_t)size, NULL};
  if (!retro_load_game_f(&info)) { fprintf(stderr, "core refused %s\n", rom); return 1; }

  struct retro_system_av_info av;
  retro_get_system_av_info_f(&av);
  g_sample_rate = av.timing.sample_rate;
  const double fps = av.timing.fps;
  const long frames = (long)(seconds * fps);
  for (long i = 0; i < frames; ++i) {
    // Oracle hooks use this to turn cores' frame-local chip timestamps into a
    // stable ROM-run timeline without coupling the frontend to a core ABI.
    char frame_text[32];
    snprintf(frame_text, sizeof frame_text, "%ld", i);
    setenv("YANES_FRAME_INDEX", frame_text, 1);
    g_frame_index = i;
    retro_run_f();
  }

  write_wav(wav, g_pcm, g_pcm_len, (uint32_t)g_sample_rate);
  printf("oracle: %s %.2fs @ %.0f Hz, %.0f fps -> %s (%zu frames)\n",
         rom, seconds, g_sample_rate, fps, wav, g_pcm_len / 2);

  retro_unload_game_f();
  retro_deinit_f();
  free(data);
  return 0;
}
