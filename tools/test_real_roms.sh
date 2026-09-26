#!/bin/sh
# Reproduce the real-game matrix; ROMs and patched cores remain external.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
roms=${1:?usage: tools/test_real_roms.sh ROM_DIRECTORY [BUILD_DIRECTORY]}
build=${2:-$root/build}
export YANES_GB_ORACLE="$build/yanes-gb-oracle"
export YANES_LIBRETRO_HOST="$build/yanes-libretro-host"
export YANES_GB_REPLAY="$build/yanes-gb-replay"
export YANES_NES_REPLAY="$build/yanes-nes-replay"
export YANES_PCE_REPLAY="$build/yanes-pce-replay"
export YANES_PARITY_COMPARE="$build/yanes-parity-compare"
passed=0
failed=0
run() {
  lane=$1; game=$2; seconds=$3; start=$4; a=$5
  folder=$lane
  if [ "$lane" = pce ]; then folder=pcengine; fi
  echo "=== $lane: $game (${seconds}s) ==="
  if YANES_AUTO_START=1 YANES_AUTO_START_FRAMES="$start" YANES_AUTO_A_FRAMES="$a" \
    "$root/tools/test_${lane}_rom_parity.sh" "$roms/$folder/$game" "$seconds"; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
  fi
}
for game in 'Super Mario Bros.' 'Super Mario Bros. 3' Castlevania \
  "Castlevania III - Dracula's Curse" 'Mega Man 2' "Kirby's Adventure" \
  Excitebike 'Ice Climber' Contra Metroid 'Ninja Gaiden' 'Duck Tales' Battletoads; do
  run nes "$game.nes" 12 '60,240' ''
done
# Register a name before selecting the new Zelda save slot.
run nes 'Legend of Zelda, The.nes' 16 '60,240,360,420' '300'
for game in 'Balloon Kid' "Kirby's Dream Land" Alleyway \
  'Metroid II - Return of Samus' "Kirby's Pinball Land"; do
  run gb "$game.gb" 12 '60,150,240,330' ''
done
# These titles are not ready for input during the default boot-time presses.
run gb 'Super Mario Land.gb' 12 '420' ''
run gb 'Battletoads.gb' 16 '420,540,660,780' ''
run gb 'Tetris DX.gbc' 14 '60,150,240,330' ''
for game in Turrican "Bonk's Adventure" 'Air Zonk' 'Time Cruise'; do
  # The original PCE captures use their title/demo audio without input.
  echo "=== pce: $game (8s) ==="
  if YANES_AUTO_START= YANES_AUTO_A_FRAMES= \
    "$root/tools/test_pce_rom_parity.sh" "$roms/pcengine/$game.pce" 8; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
  fi
done
echo "Real-ROM results: $passed passed, $failed failed"
test "$failed" -eq 0
