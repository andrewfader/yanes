#!/bin/sh
# SameBoy envelope lane: a synthetic envelope ROM (tools/yanes_gb_envelope_rom.cpp)
# runs under SameBoy with a stub boot ROM, YANES replays the register writes it
# logged, and every channel is scored two ways against SameBoy's own render:
#   envelope_oracle        amplitude envelope in dB and absolute level
#   yanes-parity-compare   timbre, envelope correlation and onset/offset
# Needs nothing outside this repo except the patched SameBoy the oracle links.
set -eu

rom_tool=${YANES_GB_ENVELOPE_ROM:-build/yanes-gb-envelope-rom}
oracle=${YANES_GB_ORACLE:-build/yanes-gb-oracle}
replay=${YANES_GB_REPLAY:-build/yanes-gb-replay}
envelope=${YANES_ENVELOPE_ORACLE:-build/envelope_oracle}
compare=${YANES_PARITY_COMPARE:-build/yanes-parity-compare}
seconds=10
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-gb-envelope.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for tool in "$rom_tool" "$oracle" "$replay" "$envelope" "$compare"; do
  test -x "$tool" || { echo "missing tool: $tool" >&2; exit 1; }
done

"$rom_tool" "$work/envelope.gb"
"$oracle" "$work/envelope.gb" "$work" "$seconds" stub
"$replay" "$work/gb_registers.log" "$work" "$seconds"

failed=0
for name in mix ch1_pulse ch2_pulse ch3_wave ch4_noise; do
  window=20; mode=rom
  case "$name" in *noise*) window=80;; mix) mode=rom-mix;; esac
  # Same chip model on both sides, so level is gated too (1.5 dB).
  if env_row=$("$envelope" "$work/yanes_$name.wav" "$work/gb_$name.wav" --level-db 1.5 --window-ms "$window"); then :; else failed=1; fi
  if par_row=$("$compare" "$work/gb_$name.wav" "$work/yanes_$name.wav" "$mode" 0.9); then :; else failed=1; fi
  echo "$name"
  echo "  $env_row"
  echo "  parity: $par_row"
done

if [ "$failed" -ne 0 ]; then
  echo "SameBoy envelope lane failed; the rows above name the channel." >&2
  exit 1
fi
echo "PASS: every Game Boy channel's envelope matches SameBoy."
