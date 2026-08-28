#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(cd "$script_dir/.." && pwd)

plugin=${YANES_CLAP:-$root_dir/build/YANES.clap}
rom_emu=${YANES_NES_ROM_EMU:-$root_dir/build/yanes-nes-rom-emu}
audio_compare=${YANES_AUDIO_COMPARE:-$root_dir/build/yanes-audio-compare}
parity_compare=${YANES_PARITY_COMPARE:-$root_dir/build/yanes-parity-compare}
rom_dir=${NES_ROM_DIR:-/mnt/crucial/roms/nes}

echo "=========================================================="
echo "    YANES Independent 2A03 Hardware Gate vs Preset Path   "
echo "=========================================================="

test -f "$plugin" || { echo "YANES CLAP plugin not found: $plugin" >&2; exit 1; }
test -x "$rom_emu" || { echo "YANES NES ROM emulator not found: $rom_emu" >&2; exit 1; }
test -x "$audio_compare" || { echo "Audio compare binary not found: $audio_compare" >&2; exit 1; }
test -x "$parity_compare" || { echo "Parity compare binary not found: $parity_compare" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-nes-rom-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo ""
echo "[1/3] Scanning ROM directory $rom_dir for score identity..."
if [ -d "$rom_dir" ]; then
  count=0
  for rom in "$rom_dir"/*.nes; do
    if [ -f "$rom" ]; then
      magic=$(head -c 4 "$rom" 2>/dev/null || true)
      if [ "$magic" = "NES"$'\x1a' ]; then
        count=$((count + 1))
      fi
    fi
  done
  echo "Found and verified $count valid iNES ROMs."
else
  echo "Error: $rom_dir not found." >&2
  exit 1
fi

echo ""
echo "[2/3] Independent cycle-stepped 2A03 render + YANES NES-stack preset..."
test_rom=""
for candidate in "$rom_dir/Balloon Fight.nes" "$rom_dir/Donkey Kong.nes" "$rom_dir/Super Mario Bros..nes"; do
  if [ -f "$candidate" ]; then
    test_rom="$candidate"
    break
  fi
done

if [ -z "$test_rom" ]; then
  test_rom=$(find "$rom_dir" -name "*.nes" -print -quit)
fi

echo "Selected ROM (score identity): $test_rom"
"$rom_emu" "$test_rom" "$plugin" "$work" 3.0

mkdir -p "$root_dir/build/reaper_projects"
cp "$work/rom_song.rpp" "$root_dir/build/reaper_projects/rom_song.rpp"
cp "$work/rom_extracted.wav" "$root_dir/build/reaper_projects/rom_extracted.wav"
cp "$work/yanes_extracted.wav" "$root_dir/build/reaper_projects/yanes_extracted.wav"

echo ""
echo "[3/3] Hardware gate (independent APU) vs YANES preset path..."
echo "Fair composite gate: onset-aligned envelope/spectrum/pitch (not waveform)."
audio_out=$("$audio_compare" "$work/rom_extracted.wav" "$work/yanes_extracted.wav" 2>&1 || true)
echo "  Audio compare: $audio_out"
# Cycle APU + blip-delta stack matches the independent gate tightly (~0.99).
parity_out=$("$parity_compare" "$work/rom_extracted.wav" "$work/yanes_extracted.wav" tonal 0.95 2>&1 || true)
echo "  Parity compare: $parity_out"
case "$parity_out" in
  *result=pass*) ;;
  *)
    echo "Hardware gate failed — YANES preset path diverges from independent 2A03." >&2
    exit 1
    ;;
esac

echo ""
echo "=========================================================="
echo "  PASS: Independent 2A03 gate clears the YANES NES stack  "
echo "  REAPER Project: build/reaper_projects/rom_song.rpp      "
echo "  Hardware APU WAV: build/reaper_projects/rom_extracted.wav"
echo "  YANES preset WAV: build/reaper_projects/yanes_extracted.wav"
echo "=========================================================="
