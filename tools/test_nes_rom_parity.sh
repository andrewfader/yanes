#!/bin/sh
# Real-ROM 2A03 hardware-oracle gate.
#
# One patched-Nestopia run of an actual .nes image yields both the reference
# audio and every CPU write to $4000-$4017. yanes-nes-replay rebuilds the
# channel state machines from that register stream alone and renders them
# through the YANES oscillator primitives, so a pass means YANES reproduces what
# the hardware produced from the same registers.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
host=${YANES_LIBRETRO_HOST:-$root/build/yanes-libretro-host}
core=${YANES_NES_CORE:-$root/../yanes-oracles/nestopia/libretro/nestopia_libretro.so}
replay=${YANES_NES_REPLAY:-$root/build/yanes-nes-replay}
compare=${YANES_PARITY_COMPARE:-$root/build/yanes-parity-compare}
rom=${1:-}
seconds=${2:-12}
# A cold NES sits in the reset routine with the APU silent; the driver only
# starts once the title screen accepts Start. Scoring that lead-in would compare
# two silences and hide whatever the music does.
warmup=${YANES_ROM_WARMUP:-4}
minimum=${YANES_ROM_MINIMUM:-0.70}

if ! awk "BEGIN{exit !($seconds > $warmup)}"; then
  echo "capture length (${seconds}s) must exceed warmup (${warmup}s)" >&2
  exit 2
fi

if [ -z "$rom" ]; then
  echo "usage: tools/test_nes_rom_parity.sh game.nes [seconds]" >&2
  exit 2
fi
for file in "$host" "$core" "$replay" "$compare" "$rom"; do
  test -e "$file" || { echo "required file not found: $file" >&2; exit 1; }
done

work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-nes-parity.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

echo "[1/3] Nestopia real-ROM audio and 2A03 register capture: $rom"
YANES_AUTO_START=${YANES_AUTO_START:-1} YANES_APU_LOG="$work/nes_apu.log" \
  "$host" "$core" "$rom" "$work/reference.wav" "$seconds"
test -s "$work/nes_apu.log" || {
  echo "core emitted no APU log; apply third_party/nestopia-apu-register-log.patch" >&2
  exit 1
}

echo "[2/3] Independent YANES 2A03 replay"
"$replay" "$work/nes_apu.log" "$work" "$seconds"

echo "[3/3] ROM composite gate (minimum $minimum, warmup ${warmup}s)"
reference=$work/reference.wav
candidate=$work/yanes_nes_mix.wav
if [ "$warmup" != 0 ] && command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -loglevel error -y -ss "$warmup" -i "$reference" "$work/scored_reference.wav"
  ffmpeg -loglevel error -y -ss "$warmup" -i "$candidate" "$work/scored_candidate.wav"
  reference=$work/scored_reference.wav
  candidate=$work/scored_candidate.wav
fi
# A ROM that never reaches its sound driver in this window would score two
# silences as a perfect match. Refuse to report that as parity: it is a capture
# problem, not evidence about the DSP.
if command -v ffmpeg >/dev/null 2>&1; then
  level=$(ffmpeg -hide_banner -nostats -i "$reference" \
    -af highpass=f=20,volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-*[0-9.]*\) dB.*/\1/p' | head -1)
  if [ -n "$level" ] && awk "BEGIN{exit !($level < -60)}"; then
    echo "reference excerpt is silent (${level} dB): the ROM did not start its" >&2
    echo "sound driver within ${seconds}s. Raise the capture length or set" >&2
    echo "YANES_AUTO_START_FRAMES to the frames this title needs." >&2
    exit 1
  fi
fi
"$compare" "$reference" "$candidate" rom "$minimum"
