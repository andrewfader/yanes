#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
oracle=${YANES_GB_ORACLE:-$root/build/yanes-gb-oracle}
replay=${YANES_GB_REPLAY:-$root/build/yanes-gb-replay}
compare=${YANES_PARITY_COMPARE:-$root/build/yanes-parity-compare}
rom=${1:-}
seconds=${2:-12}
warmup=${YANES_ROM_WARMUP:-6}
# The same two-axis bar the PC Engine and NES lanes clear.
minimum=${YANES_ROM_MINIMUM:-0.70}

if ! awk "BEGIN{exit !($seconds > $warmup)}"; then
  echo "capture length (${seconds}s) must exceed warmup (${warmup}s)" >&2
  exit 2
fi

if [ -z "$rom" ]; then
  echo "usage: tools/test_gb_rom_parity.sh game.gb [seconds]" >&2
  exit 2
fi
test -f "$rom" || { echo "Game Boy ROM not found: $rom" >&2; exit 1; }
test -x "$oracle" || { echo "Game Boy oracle not found: $oracle" >&2; exit 1; }
test -x "$replay" || { echo "Game Boy replay not found: $replay" >&2; exit 1; }
test -x "$compare" || { echo "Parity comparator not found: $compare" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-gb-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "[1/3] SameBoy ROM capture: $rom"
"$oracle" "$rom" "$work" "$seconds"
echo "[2/3] Independent YANES APU-register replay"
"$replay" "$work/gb_registers.log" "$work" "$seconds"
echo "[3/3] Per-channel hardware-oracle gate"

score_dir=$work
if command -v ffmpeg >/dev/null 2>&1 && [ "$warmup" != 0 ]; then
  score_dir=$work/scored
  mkdir -p "$score_dir"
  for side in gb yanes; do
    for name in mix ch1_pulse ch2_pulse ch3_wave ch4_noise; do
      ffmpeg -loglevel error -y -ss "$warmup" -i "$work/${side}_${name}.wav" \
        "$score_dir/${side}_${name}.wav"
    done
  done
fi

# A ROM that never reaches its sound driver in this window would score two
# silences as a perfect match. Refuse to report that as parity: it is a capture
# problem, not evidence about the DSP.
if command -v ffmpeg >/dev/null 2>&1; then
  level=$(ffmpeg -hide_banner -nostats -i "$score_dir/gb_mix.wav" \
    -af highpass=f=20,volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-*[0-9.]*\) dB.*/\1/p' | head -1)
  if [ -n "$level" ] && awk "BEGIN{exit !($level < -60)}"; then
    echo "reference excerpt is silent (${level} dB): the ROM did not start its" >&2
    echo "sound driver within ${seconds}s past the ${warmup}s warmup." >&2
    exit 1
  fi
fi

failed=0
for name in mix ch1_pulse ch2_pulse ch3_wave ch4_noise; do
  mode=rom
  if [ "$name" = mix ]; then mode=rom-mix; fi
  result=$("$compare" "$score_dir/gb_$name.wav" "$score_dir/yanes_$name.wav" "$mode" "$minimum" 2>&1 || true)
  echo "  $name: $result"
  # Every row is an acceptance gate. A channel the excerpt never uses is silent
  # on both sides and passes on that basis, so an isolated row can only fail
  # when the ROM did drive that voice and YANES rendered it differently.
  case "$result" in *result=pass*) ;; *) failed=1 ;; esac
done

if [ "$failed" -ne 0 ]; then
  echo "Game Boy hardware-oracle parity failed; the rows above name the voice." >&2
  exit 1
fi
echo "PASS: Game Boy ROM audio clears the SameBoy hardware-oracle gate."
