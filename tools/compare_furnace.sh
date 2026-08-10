#!/bin/sh
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: compare_furnace.sh CHIP module.fur [minimum-envelope-correlation]" >&2
  echo "chips: ym2203 ym2608 ym2612 ym2151 ym3812 ymf262" >&2
  exit 2
fi
chip=$1
module=$2
case "$chip" in ym2203|ym2608|ym2612|ym2151|ym3812|ymf262) ;; *) echo "unsupported chip: $chip" >&2; exit 2;; esac
case "$0" in */*) tool_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../build" 2>/dev/null && pwd || dirname -- "$0");; *) tool_dir=;; esac
renderer=${YANES_REGISTER_RENDER:-${tool_dir:+$tool_dir/}yanes-register-render}
extractor=${YANES_VGM_EXTRACT:-${tool_dir:+$tool_dir/}yanes-vgm-extract}
comparator=${YANES_AUDIO_COMPARE:-${tool_dir:+$tool_dir/}yanes-audio-compare}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-furnace.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
furnace -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -outmode persys -output "$work/reference.wav" "$module" >/dev/null 2>&1
furnace -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -vgmout "$work/reference.vgm" "$module" >/dev/null 2>&1
"$extractor" "$chip" "$work/reference.vgm" "$work/reference.reg"
"$renderer" "$chip" "$work/reference.reg" "$work/candidate.wav"
if [ "$#" -eq 3 ]; then
  "$comparator" "$work/reference_s01.wav" "$work/candidate.wav" "$3"
else
  "$comparator" "$work/reference_s01.wav" "$work/candidate.wav"
fi
