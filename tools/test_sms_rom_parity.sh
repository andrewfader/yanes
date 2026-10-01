#!/bin/sh
# Scores YANES's SN76489 model against a real SMS game: the patched Genesis Plus
# GX core renders the ROM and logs its PSG writes, YANES replays that register
# stream independently, and the two audios are compared. The patched core is
# external (apply third_party/genesis-plus-gx-psg-register-log.patch).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host=${YANES_LIBRETRO_HOST:-$root/build/yanes-libretro-host}
core=${YANES_SMS_CORE:-$root/../yanes-oracles/genesis-plus-gx/genesis_plus_gx_libretro.so}
replay=${YANES_SMS_REPLAY:-$root/build/yanes-sms-replay}
compare=${YANES_PARITY_COMPARE:-$root/build/yanes-parity-compare}
rom=${1:-}
seconds=${2:-8}
minimum=${YANES_ROM_MINIMUM:-0.70}
if [ -z "$rom" ]; then echo "usage: tools/test_sms_rom_parity.sh game.sms [seconds]" >&2; exit 2; fi
for file in "$host" "$core" "$replay" "$compare" "$rom"; do
  test -f "$file" || { echo "required file not found: $file" >&2; exit 1; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-sms-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
echo "[1/3] Genesis Plus GX real-ROM audio and SN76489 register capture"
YANES_PSG_LOG="$work/sms.log" "$host" "$core" "$rom" "$work/reference.wav" "$seconds"
test -s "$work/sms.log" || { echo "core emitted no PSG log; apply third_party/genesis-plus-gx-psg-register-log.patch" >&2; exit 1; }
echo "[2/3] Independent YANES SN76489 replay"
"$replay" "$work/sms.log" "$work" "$seconds"
echo "[3/3] ROM composite gate (minimum $minimum)"
if command -v ffmpeg >/dev/null 2>&1; then
  level=$(ffmpeg -hide_banner -nostats -i "$work/reference.wav" \
    -af highpass=f=20,volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-*[0-9.]*\) dB.*/\1/p' | head -1)
  if [ -n "$level" ] && awk "BEGIN{exit !($level < -60)}"; then
    echo "reference excerpt is silent (${level} dB): the ROM did not start its sound driver, or it uses the FM add-on." >&2
    exit 1
  fi
fi
"$compare" "$work/reference.wav" "$work/yanes_sms_mix.wav" rom-mix "$minimum"
