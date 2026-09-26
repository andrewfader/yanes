#!/bin/sh
# Additive generated-score checks; the ROM-directory and real-ROM gates remain separate.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
plugin=${YANES_CLAP:-$root/build/YANES.clap}
renderer=${YANES_NES_ROM_EMU:-$root/build/yanes-nes-rom-emu}
compare=${YANES_PARITY_COMPARE:-$root/build/yanes-parity-compare}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-nes-oscillator.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# A polyphonic generated score compares two local APU models, without ROM files.
"$renderer" --synthetic "$plugin" "$work" 3.0
"$compare" "$work/rom_extracted.wav" "$work/yanes_extracted.wav" rom-mix 0.95
for control in source duty pitch; do
  mkdir "$work/$control"
  case "$control" in
    source) set -- 1 -1 0 ;;
    duty) set -- 18 0 0 ;;
    pitch) set -- 18 -1 12 ;;
  esac
  "$renderer" --synthetic "$plugin" "$work/$control" 3.0 "$@"
  if "$compare" "$work/$control/rom_extracted.wav" "$work/$control/yanes_extracted.wav" rom-mix 0.95; then
    echo "oscillator gate accepted the wrong $control" >&2
    exit 1
  fi
done
echo "PASS: generated NES score matches; wrong source, duty, and octave are rejected."
