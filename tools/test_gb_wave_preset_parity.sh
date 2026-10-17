#!/bin/sh
# SameBoy lane for the Game Boy wave presets. Each preset carries a wave table
# harvested from a real game; for every one, yanes-gb-wave-preset-parity checks
# the preset holds the game's bytes, renders it holding C-4, and writes a script
# ROM that plays the same bytes on CH3 at the same pitch. SameBoy runs the ROM
# with a stub boot ROM (no commercial ROM needed) and its CH3 render is the
# reference for two gates:
#   yanes-parity-compare   envelope, coarse spectrum, onset/offset and pitch
#   harmonics              each of the first 16 harmonics within 1.5 dB, which
#                          tells tables apart that the coarse spectrum cannot
set -eu

tool=${YANES_GB_WAVE_PRESET_PARITY:-build/yanes-gb-wave-preset-parity}
plugin=${YANES_CLAP:-build/YANES.clap}
oracle=${YANES_GB_ORACLE:-build/yanes-gb-oracle}
compare=${YANES_PARITY_COMPARE:-build/yanes-parity-compare}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-gb-wave-presets.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for t in "$tool" "$oracle" "$compare"; do
  test -x "$t" || { echo "missing tool: $t" >&2; exit 1; }
done
test -f "$plugin" || { echo "missing plug-in: $plugin" >&2; exit 1; }

"$tool" list "$plugin" > "$work/presets" || { echo "a harvested table has no preset" >&2; exit 1; }
count=$(wc -l < "$work/presets")
test "$count" -gt 0 || { echo "no Game Boy wave presets to check" >&2; exit 1; }

failed=0
checked=0
# Nothing in the loop may read the preset list: stdin is closed for every command.
while read -r id hz; do
  dir=$work/$id
  mkdir -p "$dir"
  checked=$((checked + 1))
  if ! "$tool" render "$plugin" "$id" "$dir" < /dev/null; then failed=1; continue; fi
  "$oracle" "$dir/wave.gb" "$dir" 2 stub < /dev/null > /dev/null
  if par=$("$compare" "$dir/gb_ch3_wave.wav" "$dir/yanes.wav" tonal 0.9 < /dev/null); then :; else failed=1; fi
  echo "  parity: $par"
  if harm=$("$tool" harmonics "$dir/gb_ch3_wave.wav" "$dir/yanes.wav" "$hz" < /dev/null); then :; else failed=1; fi
  echo "$harm" | tail -1 | sed 's/^/  /'
  case "$par$harm" in *result=fail*) echo "$harm" | sed 's/^/    /' ;; esac
done < "$work/presets"

if [ "$checked" -ne "$count" ]; then
  echo "checked $checked of $count presets" >&2
  failed=1
fi
if [ "$failed" -ne 0 ]; then
  echo "Game Boy wave preset parity failed; the rows above name the preset." >&2
  exit 1
fi
echo "PASS: all $checked Game Boy wave presets play their game's table like SameBoy's CH3."
