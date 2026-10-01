// Build this file inside the Furnace source tree. It uses Furnace's GPL engine
// to regenerate the original, redistributable single-note parity modules.
//
// Most fixtures play Furnace's default instrument on the chip, untouched. A few chips have no
// sound at all with a default instrument (a sample player with no sample, a Lynx channel with no
// LFSR taps, a WonderSwan channel with noise off), or the YANES voice is defined by a chip mode
// the default instrument does not use (the AY envelope as an oscillator, a YM2413 ROM patch).
// Those fixtures set exactly what the YANES voice's own defaults select, listed with each entry,
// and nothing else; the samples are YANES's own built-in loops so both sides play the same bytes.
#include "pch.h"
#include "engine/config.h"
#include "engine/engine.h"
#include "engine/instrument.h"
#include "extra_chips.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <vector>

void reportError(String what) { std::fprintf(stderr, "%s\n", what.c_str()); }

namespace {

void macro(DivInstrumentMacro &m, int value) {
  m.len = 1;
  m.val[0] = value;
}

// An 8-bit looping sample.
void add_sample8(DivEngine &engine, const std::vector<signed char> &bytes, int rate) {
  DivSample *s = new DivSample;
  s->name = "YANES built-in loop";
  s->depth = DIV_SAMPLE_DEPTH_8BIT;
  s->init(static_cast<unsigned int>(bytes.size()));
  std::memcpy(s->data8, bytes.data(), bytes.size());
  s->centerRate = rate;
  s->loop = true;
  s->loopStart = 0;
  s->loopEnd = static_cast<int>(bytes.size());
  engine.addSamplePtr(s);
}

// A 16-bit one-shot sample.
void add_sample16(DivEngine &engine, const std::vector<short> &pcm, int rate) {
  DivSample *s = new DivSample;
  s->name = "YANES built-in sound";
  s->depth = DIV_SAMPLE_DEPTH_16BIT;
  s->init(static_cast<unsigned int>(pcm.size()));
  std::memcpy(s->data16, pcm.data(), pcm.size() * sizeof(short));
  s->centerRate = rate;
  s->loop = false;
  engine.addSamplePtr(s);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4)
    return 2;
  const int note = argc == 4 ? static_cast<int>(std::strtol(argv[3], nullptr, 10)) : 108;
  if (note < 1 || note > 179)
    return 2;
  // Instrument, samples and (for the AY buzzer) row-0 effects; `note` is the Furnace note played.
  using Setup = std::function<void(DivEngine &, DivInstrument &, DivPattern &, int note)>;
  struct System {
    const char *name;
    DivSystem system;
    int channel;
    Setup setup;
    const char *flags;  // "key=value" chip flags, or nullptr
  };
  const System systems[] = {
      {"nes-pulse", DIV_SYSTEM_NES, 0, nullptr, nullptr},
      {"nes-triangle", DIV_SYSTEM_NES, 2, nullptr, nullptr},
      {"nes-noise", DIV_SYSTEM_NES, 3, nullptr, nullptr},
      {"gameboy-pulse", DIV_SYSTEM_GB, 0, nullptr, nullptr},
      {"gameboy-wave", DIV_SYSTEM_GB, 2, nullptr, nullptr},
      {"gameboy-noise", DIV_SYSTEM_GB, 3, nullptr, nullptr},
      {"sms-tone", DIV_SYSTEM_SMS, 0, nullptr, nullptr},
      {"sms-noise", DIV_SYSTEM_SMS, 3, nullptr, nullptr},
      {"pce-wave", DIV_SYSTEM_PCE, 0, nullptr, nullptr},
      {"ay-tone", DIV_SYSTEM_AY8910, 0, nullptr, nullptr},
      {"pokey-tone", DIV_SYSTEM_POKEY, 0, nullptr, nullptr},
      {"sid6581", DIV_SYSTEM_C64_6581, 0, nullptr, nullptr},
      {"sid8580", DIV_SYSTEM_C64_8580, 0, nullptr, nullptr},
      {"scc", DIV_SYSTEM_SCC, 0, nullptr, nullptr},
      {"saa1099", DIV_SYSTEM_SAA1099, 0, nullptr, nullptr},
      {"tia", DIV_SYSTEM_TIA, 0, nullptr, nullptr},
      {"vrc6-pulse", DIV_SYSTEM_VRC6, 0, nullptr, nullptr},
      {"vrc6-saw", DIV_SYSTEM_VRC6, 2, nullptr, nullptr},
      {"fds", DIV_SYSTEM_FDS, 0, nullptr, nullptr},
      {"n163", DIV_SYSTEM_N163, 0, nullptr, nullptr},
      {"vrc7", DIV_SYSTEM_VRC7, 0, nullptr, nullptr},
      // Second wave. Default instruments unless noted.
      {"pc-speaker", DIV_SYSTEM_PCSPKR, 0, nullptr, nullptr},
      {"zx-beeper", DIV_SYSTEM_SFX_BEEPER, 0, nullptr, nullptr},
      {"msx-bass-drum", DIV_SYSTEM_OPLL_DRUMS, 6, nullptr, nullptr},
      {"msx-snare", DIV_SYSTEM_OPLL_DRUMS, 7, nullptr, nullptr},
      {"msx-tom", DIV_SYSTEM_OPLL_DRUMS, 8, nullptr, nullptr},
      {"msx-cymbal", DIV_SYSTEM_OPLL_DRUMS, 9, nullptr, nullptr},
      {"msx-hihat", DIV_SYSTEM_OPLL_DRUMS, 10, nullptr, nullptr},
      {"virtual-boy-wave", DIV_SYSTEM_VBOY, 0, nullptr, nullptr},
      {"virtual-boy-noise", DIV_SYSTEM_VBOY, 5, nullptr, nullptr},
      {"wonderswan-wave", DIV_SYSTEM_SWAN, 0, nullptr, nullptr},
      // Channel 4 plays noise only when its duty macro selects a noise mode; 1 is the first tap.
      {"wonderswan-noise", DIV_SYSTEM_SWAN, 3,
       [](DivEngine &, DivInstrument &ins, DivPattern &, int) { macro(ins.std.dutyMacro, 1); }, nullptr},
      // A Lynx channel with no taps holds a constant level; the f0 tap set is the square YANES plays.
      {"lynx", DIV_SYSTEM_LYNX, 0,
       [](DivEngine &, DivInstrument &ins, DivPattern &, int) { macro(ins.std.dutyMacro, 1); }, nullptr},
      // ROM instrument 1 (violin), which the YANES MSX voice selects by default.
      {"msx-ym2413", DIV_SYSTEM_OPLL, 0,
       [](DivEngine &, DivInstrument &ins, DivPattern &, int) { ins.fm.opllPreset = 1; }, nullptr},
      // The AY envelope as the oscillator: envelope on and tone off (wave bits), the repeating
      // falling saw (shape 8), on the 2 MHz clock YANES's AY uses, with the envelope period set to
      // the one YANES picks for the note: round(clock / (256 f)). (Furnace's auto-envelope would
      // truncate the tone period instead, which lands up to a third of a semitone sharp.) Furnace
      // note 108 is C-4 on the AY.
      {"ay-buzzer", DIV_SYSTEM_AY8910, 0,
       [](DivEngine &, DivInstrument &ins, DivPattern &pattern, int note) {
         macro(ins.std.waveMacro, 4);
         const double hz = 261.6255653 * std::pow(2.0, (note - 108) / 12.0);
         const int period = static_cast<int>(std::lround(2000000.0 / (256.0 * hz)));
         pattern.newData[0][DIV_PAT_FX(0)] = 0x22;
         pattern.newData[0][DIV_PAT_FXVAL(0)] = 0x81;
         pattern.newData[0][DIV_PAT_FX(1)] = 0x23;
         pattern.newData[0][DIV_PAT_FXVAL(1)] = period & 0xff;
         pattern.newData[0][DIV_PAT_FX(2)] = 0x24;
         pattern.newData[0][DIV_PAT_FXVAL(2)] = period >> 8;
       }, "clockSel=3"},
      // YANES's built-in saw loop (32 bytes), centred so it plays C-4 at Furnace's C-4.
      {"amiga", DIV_SYSTEM_AMIGA, 0,
       [](DivEngine &engine, DivInstrument &ins, DivPattern &, int) {
         std::vector<signed char> loop;
         for (int i = 0; i < yanes::extra::kPaulaLoop; ++i) loop.push_back(yanes::extra::paula_builtin(1, i));
         add_sample8(engine, loop, static_cast<int>(std::lround(261.6255653 * yanes::extra::kPaulaLoop)));
         ins.amiga.initSample = 0;
       }, nullptr},
      // MinMod is Furnace's GBA software mixer; YANES mixes at 13379 Hz. The sample is YANES's
      // built-in string loop (64 bytes).
      {"gba-minmod", DIV_SYSTEM_GBA_MINMOD, 0,
       [](DivEngine &engine, DivInstrument &ins, DivPattern &, int) {
         std::vector<signed char> loop;
         for (int i = 0; i < yanes::extra::kGbaLoop; ++i) loop.push_back(yanes::extra::gba_builtin(3, i));
         add_sample8(engine, loop, static_cast<int>(std::lround(261.6255653 * yanes::extra::kGbaLoop)));
         ins.amiga.initSample = 0;
       }, "sampRate=13379"},
      // YANES's built-in arcade kick, rendered at the OKI's 7575 Hz; Furnace encodes it to ADPCM.
      {"msm6295", DIV_SYSTEM_MSM6295, 0,
       [](DivEngine &engine, DivInstrument &ins, DivPattern &, int) {
         uint32_t lfsr = 1;
         double previous = 0.0;
         yanes::extra::LpcVoice speech;
         std::vector<short> pcm;
         for (double t = 0.0; t < yanes::extra::kArcadeKitLength[0]; t += 1.0 / yanes::extra::kOkiRate)
           pcm.push_back(static_cast<short>(std::lround(std::clamp(
               yanes::extra::arcade_kit(0, t, lfsr, previous, speech), -1.0, 1.0) * 32767.0)));
         add_sample16(engine, pcm, static_cast<int>(std::lround(yanes::extra::kOkiRate)));
         ins.amiga.initSample = 0;
       }, nullptr},
  };
  const System *selected = nullptr;
  for (const auto &item : systems)
    if (!std::strcmp(item.name, argv[1]))
      selected = &item;
  if (!selected)
    return 2;
  DivEngine engine;
  engine.prePreInit();
  engine.preInit(true);
  engine.setAudio(DIV_AUDIO_DUMMY);
  DivConfig preset;
  preset.set("id0", static_cast<int>(engine.systemToFileFur(selected->system)));
  preset.set("chans0", engine.getChannelCount(selected->system));
  preset.set("vol0", 1.0f);
  preset.set("pan0", 0.0f);
  if (selected->flags) {
    DivConfig flags;
    flags.loadFromMemory(selected->flags);
    preset.set("flags0", flags.toBase64());
  }
  engine.setConf("initialSys2", preset.toBase64());
  if (!engine.init())
    return 1;
  if (engine.song.ins.empty())
    engine.addInstrument(selected->channel);
  if (engine.song.wave.empty())
    engine.addWave();
  engine.song.name = "YANES parity fixture";
  engine.song.author = "YANES contributors";
  engine.song.systemName = engine.getSystemName(selected->system);
  DivSubSong *sub = engine.song.subsong[0];
  sub->patLen = 32;
  sub->ordersLen = 1;
  DivPattern *pattern = sub->pat[selected->channel].getPattern(0, true);
  pattern->newData[0][DIV_PAT_NOTE] = static_cast<short>(note);
  pattern->newData[0][DIV_PAT_INS] = 0;
  pattern->newData[16][DIV_PAT_NOTE] = DIV_NOTE_OFF;
  if (selected->setup) {
    sub->pat[selected->channel].effectCols = 3;
    selected->setup(engine, *engine.song.ins[0], *pattern, note);
  }
  SafeWriter *writer = engine.saveFur(true);
  if (!writer)
    return 1;
  FILE *output = std::fopen(argv[2], "wb");
  if (!output) {
    writer->finish();
    return 1;
  }
  const bool ok =
      std::fwrite(writer->getFinalBuf(), 1, writer->size(), output) == writer->size();
  std::fclose(output);
  writer->finish();
  _exit(ok ? 0 : 1);
}
