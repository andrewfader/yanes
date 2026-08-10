#!/bin/sh
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then echo "usage: compare_furnace_audio.sh FIXTURE_NAME fixture.fur [minimum-correlation]" >&2; exit 2; fi
name=$1; module=$2; minimum=${3:-0.8}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-furnace-audio.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
"${FURNACE:-furnace}" -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -output "$work/furnace.wav" "$module" >/dev/null 2>&1
"${YANES_FIXTURE_RENDER:-build/yanes-fixture-render}" "${YANES_CLAP:-build/YANES.clap}" "$name" "$work/yanes.wav"
case "$name" in *noise*) kind=noise;; *) kind=tonal;; esac
if [ "${YANES_COMPOSITE_PARITY:-1}" = 1 ]; then
  "${YANES_PARITY_COMPARE:-build/yanes-parity-compare}" "$work/furnace.wav" "$work/yanes.wav" "$kind" "$minimum"
else
  "${YANES_AUDIO_COMPARE:-build/yanes-audio-compare}" "$work/furnace.wav" "$work/yanes.wav" "$minimum"
fi
