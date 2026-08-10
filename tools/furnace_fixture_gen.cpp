// Build this file inside the Furnace source tree. It uses Furnace's GPL engine
// to regenerate the original, redistributable single-note parity modules.
#include "pch.h"
#include "engine/engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

void reportError(String what) { std::fprintf(stderr, "%s\n", what.c_str()); }

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) return 2;
  const int note = argc == 4 ? std::strtol(argv[3], nullptr, 10) : 108;
  if (note < 1 || note > 179) return 2;
  struct System { const char *name; DivSystem system; int channel; };
  const System systems[] = {
    {"nes-pulse",DIV_SYSTEM_NES,0},{"nes-triangle",DIV_SYSTEM_NES,2},{"nes-noise",DIV_SYSTEM_NES,3},
    {"gameboy-pulse",DIV_SYSTEM_GB,0},{"gameboy-wave",DIV_SYSTEM_GB,2},{"gameboy-noise",DIV_SYSTEM_GB,3},
    {"sms-tone",DIV_SYSTEM_SMS,0},{"sms-noise",DIV_SYSTEM_SMS,3},{"pce-wave",DIV_SYSTEM_PCE,0},
    {"ay-tone",DIV_SYSTEM_AY8910,0},{"pokey-tone",DIV_SYSTEM_POKEY,0},
    {"sid6581",DIV_SYSTEM_C64_6581,0},{"sid8580",DIV_SYSTEM_C64_8580,0},{"scc",DIV_SYSTEM_SCC,0},
    {"saa1099",DIV_SYSTEM_SAA1099,0},{"tia",DIV_SYSTEM_TIA,0},{"vrc6-pulse",DIV_SYSTEM_VRC6,0},
    {"vrc6-saw",DIV_SYSTEM_VRC6,2},{"fds",DIV_SYSTEM_FDS,0},{"n163",DIV_SYSTEM_N163,0},{"vrc7",DIV_SYSTEM_VRC7,0}
  };
  const System *selected = nullptr;
  for (const auto &item : systems) if (!std::strcmp(item.name, argv[1])) selected = &item;
  if (!selected) return 2;
  DivEngine engine; engine.prePreInit(); engine.preInit(true); if (!engine.init()) return 1;
  engine.song.systemLen=1; engine.song.system[0]=selected->system;
  engine.song.systemChans[0]=engine.getChannelCount(selected->system); engine.song.systemVol[0]=1.0f;
  engine.song.systemPan[0]=0.0f; engine.song.systemPanFR[0]=0.0f;
  engine.song.name="YANES parity fixture"; engine.song.author="YANES contributors";
  engine.song.systemName=engine.getSystemName(selected->system); engine.song.recalcChans();
  DivSubSong *sub=engine.song.subsong[0]; sub->patLen=32; sub->ordersLen=1;
  DivPattern *pattern=sub->pat[selected->channel].getPattern(0,true);
  pattern->newData[0][DIV_PAT_NOTE]=note; pattern->newData[16][DIV_PAT_NOTE]=DIV_NOTE_OFF;
  SafeWriter *writer=engine.saveFur(true); if (!writer) return 1;
  FILE *output=std::fopen(argv[2],"wb"); if (!output) { writer->finish(); return 1; }
  const bool ok=std::fwrite(writer->getFinalBuf(),1,writer->size(),output)==writer->size();
  std::fclose(output); writer->finish(); return ok ? 0 : 1;
}
