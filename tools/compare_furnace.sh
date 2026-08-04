#!/bin/sh
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: compare_furnace.sh module.fur [minimum-envelope-correlation]" >&2
  exit 2
fi
case "$0" in */*) tool_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../build" 2>/dev/null && pwd || dirname -- "$0");; *) tool_dir=;; esac
renderer=${YANES_REGISTER_RENDER:-${tool_dir:+$tool_dir/}yanes-register-render}
extractor=${YANES_VGM_EXTRACT:-${tool_dir:+$tool_dir/}yanes-vgm-extract}
comparator=${YANES_AUDIO_COMPARE:-${tool_dir:+$tool_dir/}yanes-audio-compare}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-furnace.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
furnace -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -outmode persys -output "$work/reference.wav" "$1" >/dev/null 2>&1
furnace -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -vgmout "$work/reference.vgm" "$1" >/dev/null 2>&1
"$extractor" "$work/reference.vgm" "$work/reference.reg"
"$renderer" ym2612 "$work/reference.reg" "$work/candidate.wav"
if [ "$#" -eq 2 ]; then
  "$comparator" "$work/reference_s01.wav" "$work/candidate.wav" "$2"
else
  "$comparator" "$work/reference_s01.wav" "$work/candidate.wav"
fi
