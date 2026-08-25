#!/bin/sh
set -eu

furnace_bin=${YANES_FURNACE_EXECUTABLE:-build/furnace}
plugin=${YANES_CLAP:-build/YANES.clap}
parity_compare=${YANES_PARITY_COMPARE:-build/yanes-parity-compare}
fixture_render=${YANES_FIXTURE_RENDER:-build/yanes-fixture-render}
rom_dir=${NES_ROM_DIR:-/mnt/crucial/roms/nes}

echo "=========================================================="
echo "    YANES NES Emulator & ROM Audio Parity Test Suite      "
echo "=========================================================="

test -x "$furnace_bin" || { echo "Furnace 2A03 engine not found: $furnace_bin" >&2; exit 1; }
test -f "$plugin" || { echo "YANES CLAP plugin not found: $plugin" >&2; exit 1; }
test -x "$parity_compare" || { echo "Parity compare binary not found: $parity_compare" >&2; exit 1; }
test -x "$fixture_render" || { echo "Fixture render binary not found: $fixture_render" >&2; exit 1; }

echo ""
echo "[1/3] Scanning and validating NES ROMs in $rom_dir ..."
if [ -d "$rom_dir" ]; then
  count=0
  for rom in "$rom_dir"/*.nes; do
    if [ -f "$rom" ]; then
      # Check iNES magic bytes: 'NES\x1a'
      magic=$(head -c 4 "$rom" 2>/dev/null || true)
      if [ "$magic" = "NES"$'\x1a' ]; then
        count=$((count + 1))
      fi
    fi
  done
  echo "Found and verified $count valid iNES ROMs in $rom_dir."
  
  # Highlight notable reference ROMs
  for game in "Super Mario Bros..nes" "Mega Man 2.nes" "Castlevania.nes" "Legend of Zelda, The.nes" "Akumajou Densetsu.nes"; do
    if [ -f "$rom_dir/$game" ]; then
      echo "  - Reference backup detected: $game"
    fi
  done
else
  echo "Notice: $rom_dir not mounted; continuing with built-in 2A03 hardware fixtures."
fi

echo ""
echo "[2/3] Running 2A03 NES APU Channel Parity Suite vs Emulator..."
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-nes-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

run_channel_test() {
  fixture_name=$1
  fur_file=$2
  kind=$3
  min_score=$4

  echo -n "  Testing $fixture_name ... "
  
  # Render reference with Furnace 2A03 engine
  "$furnace_bin" -console -noreport -nostatus -nocontrols -loglevel error -loops 0 \
    -output "$work/ref_${fixture_name}.wav" "$fur_file" >"$work/furnace.log" 2>&1 || {
      echo "FAILED to render reference"
      cat "$work/furnace.log"
      return 1
    }

  # Render with YANES CLAP plugin
  "$fixture_render" "$plugin" "$fixture_name" "$work/yanes_${fixture_name}.wav" >/dev/null 2>&1 || {
    echo "FAILED to render YANES"
    return 1
  }

  # Evaluate audio parity
  out=$("$parity_compare" "$work/ref_${fixture_name}.wav" "$work/yanes_${fixture_name}.wav" "$kind" "$min_score" 2>&1)
  echo "PASS ($out)"
}

# Run primary NES channels
run_channel_test "nes-pulse" "tests/furnace/nes-pulse.fur" "tonal" "0.8"
run_channel_test "nes-triangle" "tests/furnace/nes-triangle.fur" "tonal" "0.8"
run_channel_test "nes-noise" "tests/furnace/nes-noise.fur" "noise" "0.8"

# Run untuned octave holdouts (high and low)
run_channel_test "nes-pulse-low" "tests/furnace_holdout/nes-pulse-low.fur" "tonal" "0.8"
run_channel_test "nes-pulse-high" "tests/furnace_holdout/nes-pulse-high.fur" "tonal" "0.8"
run_channel_test "nes-triangle-low" "tests/furnace_holdout/nes-triangle-low.fur" "tonal" "0.8"
run_channel_test "nes-triangle-high" "tests/furnace_holdout/nes-triangle-high.fur" "tonal" "0.8"
run_channel_test "nes-noise-low" "tests/furnace_holdout/nes-noise-low.fur" "noise" "0.8"
run_channel_test "nes-noise-high" "tests/furnace_holdout/nes-noise-high.fur" "noise" "0.8"

# Run expansion chip parity (VRC6 pulse and saw for Akumajou Densetsu, VRC7, FDS, N163)
echo ""
echo "[3/3] Running NES Expansion Chip Parity Suite..."
run_channel_test "vrc6-pulse" "tests/furnace/vrc6-pulse.fur" "tonal" "0.8"
run_channel_test "vrc6-saw" "tests/furnace/vrc6-saw.fur" "tonal" "0.8"
run_channel_test "fds" "tests/furnace/fds.fur" "tonal" "0.8"
run_channel_test "n163" "tests/furnace/n163.fur" "tonal" "0.8"
run_channel_test "vrc7" "tests/furnace/vrc7.fur" "tonal" "0.8"

echo ""
echo "=========================================================="
echo "    ALL NES & Expansion Chip Parity Tests Passed 100%!   "
echo "=========================================================="
