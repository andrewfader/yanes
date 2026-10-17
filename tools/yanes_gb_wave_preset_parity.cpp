// SameBoy parity for the Game Boy wave presets: each preset's wave table was
// harvested from a real game (tools/yanes_gb_wave_harvest.c, tools/gb_wave_tables.py),
// and this tool proves the preset holds that table and plays it the way the
// hardware does.
//
//   list <clap>                   id and pitch of each preset this lane covers
//   render <clap> <id> <out_dir>  check the preset's parameters against the game's
//                                 table, render it holding C-4 to <out_dir>/yanes.wav,
//                                 and write <out_dir>/wave.gb, a script ROM that plays
//                                 the same table at the same pitch on CH3
//   harmonics <ref> <cand> <hz>   compare the first 16 harmonics of <hz> in two renders
//
// The tables below are the game's wave-RAM bytes as the harvest logged them, not
// read back from the plug-in, so a preset that drifts from its game fails here.
// tools/test_gb_wave_preset_parity.sh runs the oracle between `render` and the
// comparisons.

#include "clap_harness.hpp"
#include "gb_script_rom.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace harness;

namespace {

struct GameTable {
  const char* preset;  // the preset's name, which is how the lane finds its id
  const char* game;
  const char* wave_ram;  // FF30-FF3F, high nibble first
};

constexpr GameTable kTables[] = {
    {"Pokemon Red round wave", "Pokemon Red/Blue/Yellow", "1369bdeeeeffffeddeffffeeeedb9631"},
    {"Pokemon Crystal soft saw", "Pokemon Crystal (also Red/Blue/Yellow)", "02468acefffeeddccba9876544332211"},
    {"Pokemon Crystal hollow wave", "Pokemon Crystal, Pokemon Pinball", "0011223344332211ffeeccaa88aaccee"},
    {"Pokemon Crystal buzz wave", "Pokemon Crystal", "c0a987f5fffeeddc443322f102468ace"},
    {"Tetris Blast square wave", "Tetris Blast (also Final Fantasy Adventure, Mega Man Xtreme, Tetris DX)", "ffffffff0000000000000000ffffffff"},
    {"Link's Awakening wave bass", "The Legend of Zelda: Link's Awakening (DX)", "ffffeeddeeddeeffffc963210000048c"},
    {"Kirby's Dream Land 2 wave", "Kirby's Dream Land 2 (also Kirby's Pinball Land, Kirby's Star Stacker)", "2266aaeefffffeeaa6668acffeeca633"},
    {"Gargoyle's Quest thin wave", "Gargoyle's Quest", "ff334477aacceeffffeeccbb886644ff"},
    {"Gargoyle's Quest bright wave", "Gargoyle's Quest", "ffbb77bbddeeffffffffffeeccaaffaa"},
    {"Gargoyle's Quest hollow wave", "Gargoyle's Quest", "ccaa88aaddeeffffffeeddaa99aaccee"},
    {"Mega Man Xtreme wave lead", "Mega Man Xtreme", "bdeefeedba88788a4567776542100012"},
    {"Mega Man Xtreme buzz wave", "Mega Man Xtreme", "bdeecaa6579bcca85335689975310244"},
    {"Final Fantasy Adventure pulse wave", "Final Fantasy Adventure", "ffffffff000000000000000000000000"},
    {"Metroid II wave", "Metroid II: Return of Samus", "aaaaaa000000aaaaaa000000aa00aa00"},
    {"Wario Land spike wave", "Wario Land: Super Mario Land 3", "777751a2800000000000000000000000"},
    {"Mega Man V wave", "Mega Man V", "02468aceffffffffeca8642000448844"},
    {"Oracle of Ages soft wave", "The Legend of Zelda: Oracle of Ages", "00001112223334444443332221110000"},
};

constexpr int kNote = 60;  // C-4
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kHeldBlocks = 112;  // 1.195 s, plus a 20 ms release: the ROM cuts at 1.2 s
constexpr int kTotalBlocks = 188;  // 2.0 s

std::vector<int> table_levels(const char* hex) {
  std::vector<int> out;
  for (const char* c = hex; *c; ++c) out.push_back(static_cast<int>(std::strtol(std::string(1, *c).c_str(), nullptr, 16)));
  return out;
}

// CH3 walks its 32 samples at 65536 / (2048 - period) Hz; same rounding as the
// plug-in's register grid.
int ch3_period() {
  const double note_hz = 440.0 * std::pow(2.0, (kNote - 69) / 12.0);
  return static_cast<int>(std::clamp(std::round(2048.0 - 65536.0 / note_hz), 0.0, 2047.0));
}

const GameTable* table_for(const char* preset_name) {
  for (const auto& t : kTables)
    if (std::strcmp(t.preset, preset_name) == 0) return &t;
  return nullptr;
}

void put16(std::ofstream& out, uint16_t v) { out.put(static_cast<char>(v)); out.put(static_cast<char>(v >> 8)); }
void put32(std::ofstream& out, uint32_t v) { put16(out, static_cast<uint16_t>(v)); put16(out, static_cast<uint16_t>(v >> 16)); }

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm) {
  std::ofstream out(path, std::ios::binary);
  out.write("RIFF", 4); put32(out, static_cast<uint32_t>(pcm.size() * 2 + 36)); out.write("WAVEfmt ", 8);
  put32(out, 16); put16(out, 1); put16(out, 2); put32(out, 48000); put32(out, 192000); put16(out, 4); put16(out, 16);
  out.write("data", 4); put32(out, static_cast<uint32_t>(pcm.size() * 2));
  out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
  return static_cast<bool>(out);
}

// Mono 16-bit PCM from the plain WAVs the oracle and `render` write.
std::vector<double> read_mono(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
  std::vector<double> x;
  for (size_t p = 12; p + 8 <= b.size();) {
    const uint32_t size = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | (static_cast<uint32_t>(b[p + 7]) << 24);
    if (std::memcmp(&b[p], "data", 4) == 0) {
      for (size_t q = p + 8; q + 3 < p + 8 + size && q + 3 < b.size(); q += 4) {
        const int16_t l = static_cast<int16_t>(b[q] | (b[q + 1] << 8));
        const int16_t r = static_cast<int16_t>(b[q + 2] | (b[q + 3] << 8));
        x.push_back((l + r) / 65536.0);
      }
      break;
    }
    p += 8 + size + (size & 1);
  }
  return x;
}

int render(const char* clap, int id, const std::string& dir) {
  const Library library(clap);
  const clap_plugin_t* plugin = library.create();
  const clap_id preset = find_param(plugin, "Preset");
  char name[128]{};
  params_of(plugin)->value_to_text(plugin, preset, id, name, sizeof name);
  const GameTable* table = table_for(name);
  if (!table) { std::fprintf(stderr, "preset %d (%s) has no harvested game table\n", id, name); return 1; }
  set_param(plugin, preset, id);

  // The preset has to be the game's table on CH3, at the played key.
  int failures = 0;
  auto expect = [&](bool ok, const std::string& what) {
    if (!ok) { std::fprintf(stderr, "%s: %s\n", name, what.c_str()); ++failures; }
  };
  expect(param(plugin, find_param(plugin, "Waveform")) == 11, "plays Game Boy wave (CH3)");
  expect(param(plugin, find_param(plugin, "Custom wave")) >= 0.5, "plays the drawn table");
  expect(param(plugin, find_param(plugin, "Hardware envelope")) < 0.5, "has no volume envelope, like CH3");
  expect(param(plugin, find_param(plugin, "Transpose")) == 0, "plays at the key, untransposed");
  const std::vector<int> levels = table_levels(table->wave_ram);
  for (int i = 0; i < 32; ++i) {
    char label[32];
    std::snprintf(label, sizeof label, "Wave sample %d", i + 1);
    const double level = param(plugin, find_param(plugin, label));
    expect(level == levels[static_cast<size_t>(i)],
           std::string(label) + " is " + std::to_string(static_cast<int>(level)) + ", " + table->game + " has " +
               std::to_string(levels[static_cast<size_t>(i)]));
  }

  std::vector<int16_t> pcm;
  {
    Runner runner(plugin, kRate, kBlock);
    for (int block = 0; block < kTotalBlocks; ++block) {
      Events events;
      if (block == 0) events.push(note_event(CLAP_EVENT_NOTE_ON, 0, kNote, 1, 1));
      if (block == kHeldBlocks) events.push(note_event(CLAP_EVENT_NOTE_OFF, 0, kNote, 1, 0));
      runner.run((block == 0 || block == kHeldBlocks) ? &events : nullptr);
      for (int i = 0; i < kBlock; ++i)
        for (float s : {runner.left()[static_cast<size_t>(i)], runner.right()[static_cast<size_t>(i)]})
          pcm.push_back(static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767));
    }
  }
  plugin->destroy(plugin);
  if (!write_wav(dir + "/yanes.wav", pcm)) return 1;

  // The same note on hardware: the table loaded with the DAC off, full NR32
  // volume, triggered at the period for C-4, held for
  // the same 1.2 s and cut by switching the DAC off.
  using namespace gb_script_rom;
  Script s;
  s.write(NR52, 0x80); s.write(NR51, 0xff); s.write(NR50, 0x77);
  s.write(NR30, 0x00);
  for (int i = 0; i < 16; ++i)
    s.write(static_cast<uint8_t>(WAVE + i), static_cast<uint8_t>(levels[static_cast<size_t>(2 * i)] << 4 | levels[static_cast<size_t>(2 * i + 1)]));
  const int period = ch3_period();
  s.write(NR30, 0x80); s.write(NR31, 0x00); s.write(NR32, 0x20);
  s.write(NR33, static_cast<uint8_t>(period & 0xff)); s.write(NR34, static_cast<uint8_t>(0x80 | (period >> 8)));
  s.wait(120);
  s.write(NR30, 0x00);
  s.wait(80);
  if (!write_rom((dir + "/wave.gb").c_str(), "YANES WAVE", s.finish())) return 1;
  std::printf("%s (%s): %s, CH3 period %d\n", name, table->game, failures ? "parameters FAIL" : "parameters match the game", period);
  return failures ? 1 : 0;
}

// Magnitude of each harmonic of `hz` over the held part of the note, in dB
// below the strongest one. A Hann-windowed DFT at exactly k*hz, so neither
// render's phase or start offset matters.
std::vector<double> harmonic_profile(const std::vector<double>& x, double hz) {
  const size_t first = static_cast<size_t>(0.35 * kRate), last = std::min(x.size(), static_cast<size_t>(1.15 * kRate));
  std::vector<double> mag;
  for (int k = 1; k <= 16; ++k) {
    double re = 0, im = 0;
    for (size_t n = first; n < last; ++n) {
      const double w = 0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(n - first) / static_cast<double>(last - first));
      const double ph = 2 * M_PI * k * hz * static_cast<double>(n) / kRate;
      re += w * x[n] * std::cos(ph);
      im -= w * x[n] * std::sin(ph);
    }
    mag.push_back(std::hypot(re, im));
  }
  const double peak = *std::max_element(mag.begin(), mag.end());
  for (double& m : mag) m = 20 * std::log10(std::max(m / std::max(peak, 1e-30), 1e-6));
  return mag;
}

int harmonics(const char* ref_path, const char* cand_path, double hz) {
  const auto ref = harmonic_profile(read_mono(ref_path), hz), cand = harmonic_profile(read_mono(cand_path), hz);
  // Harmonics within 30 dB of the strongest carry the timbre; below that, the
  // hardware's output filter and the plug-in's anti-alias oversampling may
  // differ without changing what the table sounds like.
  constexpr double kAudible = -30.0, kTolerance = 1.5;
  double worst = 0;
  int worst_k = 0;
  std::printf("harmonic  sameboy_db  yanes_db\n");
  for (size_t k = 0; k < ref.size(); ++k) {
    std::printf("%8zu  %10.2f  %8.2f\n", k + 1, ref[k], cand[k]);
    if (ref[k] < kAudible && cand[k] < kAudible) continue;
    const double diff = std::abs(std::max(ref[k], kAudible - 10) - std::max(cand[k], kAudible - 10));
    if (diff > worst) { worst = diff; worst_k = static_cast<int>(k + 1); }
  }
  const bool ok = worst <= kTolerance;
  std::printf("worst_harmonic=%d worst_db=%.2f tolerance_db=%.1f result=%s\n", worst_k, worst, kTolerance, ok ? "pass" : "fail");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "list") == 0) {
    // Presets are found by name so the lane covers every row of kTables and
    // fails on a row whose preset was renamed or dropped.
    const Library library(argv[2]);
    const clap_plugin_t* plugin = library.create();
    const clap_id preset = find_param(plugin, "Preset");
    const int count = static_cast<int>(param_info(plugin, preset).max_value);
    int found = 0;
    for (const auto& t : kTables) {
      int id = -1;
      for (int i = 1; i <= count && id < 0; ++i) {
        char name[128]{};
        params_of(plugin)->value_to_text(plugin, preset, i, name, sizeof name);
        if (std::strcmp(name, t.preset) == 0) id = i;
      }
      if (id < 0) { std::fprintf(stderr, "no preset is named %s\n", t.preset); continue; }
      // The pitch CH3 really plays: C-4 rounded onto the register grid. Measuring
      // harmonics at the nominal key instead smears them, more so the higher they are.
      std::printf("%d %.6f\n", id, 65536.0 / (2048 - ch3_period()));
      ++found;
    }
    plugin->destroy(plugin);
    return found == static_cast<int>(std::size(kTables)) ? 0 : 1;
  }
  if (argc == 5 && std::strcmp(argv[1], "render") == 0) return render(argv[2], std::atoi(argv[3]), argv[4]);
  if (argc == 5 && std::strcmp(argv[1], "harmonics") == 0) return harmonics(argv[2], argv[3], std::atof(argv[4]));
  std::fprintf(stderr,
               "usage: yanes-gb-wave-preset-parity list <clap>\n"
               "       yanes-gb-wave-preset-parity render <clap> <preset-id> <out_dir>\n"
               "       yanes-gb-wave-preset-parity harmonics <ref.wav> <cand.wav> <hz>\n");
  return 2;
}
