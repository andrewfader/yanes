#!/bin/sh
# Renders one Furnace fixture through Furnace and through YANES, then scores the
# pair twice: yanes-parity-compare for timbre, pitch and onset/offset, and
# envelope_oracle for the amplitude envelope in dB (attack, decay, release).
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then echo "usage: compare_furnace_audio.sh FIXTURE_NAME fixture.fur [minimum-correlation]" >&2; exit 2; fi
name=$1; module=$2; minimum=${3:-0.8}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-furnace-audio.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
if ! "${FURNACE:-furnace}" -console -noreport -nostatus -nocontrols -loglevel error -loops 0 -output "$work/furnace.wav" "$module" >"$work/furnace.log" 2>&1; then
  echo "furnace failed to render $module" >&2
  cat "$work/furnace.log" >&2
  exit 1
fi
if [ ! -s "$work/furnace.wav" ]; then
  echo "furnace produced no audio for $module" >&2
  cat "$work/furnace.log" >&2
  exit 1
fi
"${YANES_FIXTURE_RENDER:-build/yanes-fixture-render}" "${YANES_CLAP:-build/YANES.clap}" "$name" "$work/yanes.wav"
# Noise LFSRs can hold one value for tens of milliseconds, which a 20 ms window
# reads as a dropout; score noise envelopes over 80 ms.
# The YM2413's snare, cymbal and hi-hat are noise-based drums; score them as noise too.
case "$name" in *noise*|msx-snare*|msx-cymbal*|msx-hihat*) kind=noise; window=80;; *) kind=tonal; window=20;; esac
status=0
if [ "${YANES_COMPOSITE_PARITY:-1}" = 1 ]; then
  "${YANES_PARITY_COMPARE:-build/yanes-parity-compare}" "$work/furnace.wav" "$work/yanes.wav" "$kind" "$minimum" || status=1
else
  "${YANES_AUDIO_COMPARE:-build/yanes-audio-compare}" "$work/furnace.wav" "$work/yanes.wav" "$minimum" || status=1
fi
"${YANES_ENVELOPE_ORACLE:-build/envelope_oracle}" "$work/yanes.wav" "$work/furnace.wav" --window-ms "$window" || status=1
exit "$status"
