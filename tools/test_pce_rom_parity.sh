#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host=${YANES_LIBRETRO_HOST:-$root/build/yanes-libretro-host}
core=${YANES_PCE_CORE:-$root/../yanes-oracles/beetle-pce/mednafen_pce_libretro.so}
replay=${YANES_PCE_REPLAY:-$root/build/yanes-pce-replay}
compare=${YANES_PARITY_COMPARE:-$root/build/yanes-parity-compare}
rom=${1:-}
seconds=${2:-8}
minimum=${YANES_ROM_MINIMUM:-0.70}
if [ -z "$rom" ]; then echo "usage: tools/test_pce_rom_parity.sh game.pce [seconds]" >&2; exit 2; fi
for file in "$host" "$core" "$replay" "$compare" "$rom"; do
  test -f "$file" || { echo "required file not found: $file" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-pce-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
echo "[1/3] Beetle PCE real-ROM audio and HuC6280 register capture"
YANES_PSG_LOG="$work/pce.log" "$host" "$core" "$rom" "$work/reference.wav" "$seconds"
test -s "$work/pce.log" || { echo "core emitted no PSG log; apply third_party/beetle-pce-psg-register-log.patch" >&2; exit 1; }
echo "[2/3] Independent YANES HuC6280 replay"
"$replay" "$work/pce.log" "$work" "$seconds"
echo "[3/3] ROM composite gate (minimum $minimum)"
if command -v ffmpeg >/dev/null 2>&1; then
  level=$(ffmpeg -hide_banner -nostats -i "$work/reference.wav" \
    -af highpass=f=20,volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-*[0-9.]*\) dB.*/\1/p' | head -1)
  if [ -n "$level" ] && awk "BEGIN{exit !($level < -60)}"; then
    echo "reference excerpt is silent (${level} dB): the ROM did not start its sound driver." >&2
    exit 1
  fi
fi
"$compare" "$work/reference.wav" "$work/yanes_pce_mix.wav" rom-mix "$minimum"
