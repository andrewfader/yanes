#!/bin/sh
# Regenerate tests/furnace and tests/furnace_holdout from a Furnace source tree.
# Usage: FURNACE_SRC=/path/to/furnace tools/regen_furnace_fixtures.sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src=${FURNACE_SRC:?set FURNACE_SRC to a Furnace git checkout}
gen=${FURNACE_FIXTURE_GEN:-$src/build/yanes-furnace-fixture-gen}
if [ ! -x "$gen" ]; then
  echo "missing fixture generator: $gen" >&2
  exit 2
fi

# Every chip uses Furnace note 108. Furnace applies its own per-chip octave
# convention on top of that, and the resulting pitch is exactly the key each
# fixture's entry in tests/furnace_render.cpp plays (C-5 for the NES pulse,
# C-3 for the Game Boy wave, C-6 for the SMS tone, and so on). Do not hand-tune
# these per chip: a note that renders a different octave than the YANES key
# turns the parity suite into an octave-comparison instead of a timbre one.
note=108
set -- \
  nes-pulse nes-triangle nes-noise \
  gameboy-pulse gameboy-wave gameboy-noise \
  sms-tone sms-noise pce-wave \
  ay-tone pokey-tone \
  sid6581 sid8580 scc \
  saa1099 tia vrc6-pulse \
  vrc6-saw fds n163 vrc7

# The generator writes the module and then hangs in Furnace engine shutdown, so
# wait for the file to appear and settle, then kill it. Judge the run by the
# module it produced rather than by its exit status.
generate() {
  rm -f "$2"
  "$gen" "$1" "$2" "$3" >/dev/null 2>&1 &
  pid=$!
  size=0
  waited=0
  limit=${FURNACE_FIXTURE_GEN_TIMEOUT:-30}
  while [ "$waited" -lt "$((limit * 20))" ]; do
    if [ -f "$2" ]; then current=$(wc -c <"$2"); else current=0; fi
    if [ "$current" -gt 0 ] && [ "$current" -eq "$size" ]; then
      break
    fi
    size=$current
    waited=$((waited + 1))
    sleep 0.05
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  if [ ! -s "$2" ]; then
    echo "fixture generator produced nothing for $1 note $3" >&2
    exit 1
  fi
}

mkdir -p "$root/tests/furnace" "$root/tests/furnace_holdout"
for name in "$@"; do
  generate "$name" "$root/tests/furnace/$name.fur" "$note"
  generate "$name" "$root/tests/furnace_holdout/$name-low.fur" $((note - 12))
  generate "$name" "$root/tests/furnace_holdout/$name-high.fur" $((note + 12))
done
echo "regenerated 21 fixtures and 42 holdouts"
