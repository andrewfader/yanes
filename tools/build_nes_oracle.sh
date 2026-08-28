#!/bin/sh
# Builds the patched Nestopia libretro core used as the NES hardware oracle.
#   NESTOPIA_SRC=/path/to/nestopia tools/build_nes_oracle.sh
# Apply third_party/nestopia-apu-register-log.patch to the checkout first.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src=${NESTOPIA_SRC:-$root/../yanes-oracles/nestopia}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

test -f "$src/source/core/NstCpu.cpp" || {
  echo "Nestopia source not found at $src (set NESTOPIA_SRC)" >&2; exit 1;
}
grep -q YanesLogApuWrite "$src/source/core/NstCpu.cpp" || {
  echo "Nestopia checkout is missing the YANES register hook." >&2
  echo "Apply third_party/nestopia-apu-register-log.patch in $src first." >&2
  exit 1
}

make -C "$src/libretro" -j"$jobs" >/dev/null
echo "built $src/libretro/nestopia_libretro.so"
