#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
include=${LIBRETRO_INCLUDE:-$root/../yanes-oracles/beetle-pce/libretro-common/include}
out=${1:-$root/build/yanes-libretro-host}
test -f "$include/libretro.h" || {
  echo "libretro.h not found under $include (set LIBRETRO_INCLUDE)" >&2; exit 1;
}
mkdir -p "$(dirname "$out")"
cc -O2 -std=gnu11 -I"$include" "$root/tools/yanes_libretro_host.c" -o "$out" -ldl
echo "built $out"
