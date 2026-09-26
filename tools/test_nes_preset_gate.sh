#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(cd "$script_dir/.." && pwd)
plugin=${YANES_CLAP:-$root_dir/build/YANES.clap}
score_render=${YANES_NES_ROM_EMU:-$root_dir/build/yanes-nes-rom-emu}
compare=${YANES_PARITY_COMPARE:-$root_dir/build/yanes-parity-compare}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-nes-score.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
# This is a generated-score regression, not a ROM capture or a silicon oracle.
"$score_render" --synthetic "$plugin" "$work" 3.0
"$compare" "$work/rom_extracted.wav" "$work/yanes_extracted.wav" rom-mix 0.95
# The gate must also reject a deliberately wrong source using the same score.
mkdir "$work/wrong"
"$score_render" --synthetic "$plugin" "$work/wrong" 3.0 1 >"$work/wrong.log"
if "$compare" "$work/wrong/rom_extracted.wav" "$work/wrong/yanes_extracted.wav" rom-mix 0.95; then
  echo "synthetic-score gate accepted the wrong oscillator" >&2
  exit 1
fi
echo "PASS: generated NES score matches the separate APU model; wrong-source control rejected."
