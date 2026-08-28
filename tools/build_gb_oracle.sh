#!/bin/sh
# Builds yanes-gb-oracle against a patched SameBoy checkout.
#   SAMEBOY_SRC=/path/to/SameBoy tools/build_gb_oracle.sh [output]
# Apply third_party/sameboy-apu-register-log.patch to the checkout first.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src=${SAMEBOY_SRC:-$root/../yanes-oracles/sameboy}
out=${1:-$root/build/yanes-gb-oracle}
obj=${OBJ_DIR:-${TMPDIR:-/tmp}/yanes-gb-oracle-obj}

test -f "$src/Core/apu.c" || { echo "SameBoy source not found at $src" >&2; exit 1; }
grep -q GB_apu_register_callback "$src/Core/apu.h" || {
  echo "SameBoy checkout is missing the YANES register hook." >&2
  echo "Apply third_party/sameboy-apu-register-log.patch in $src first." >&2
  exit 1
}

mkdir -p "$obj" "$(dirname "$out")"
for f in "$src"/Core/*.c; do
  name=$(basename "$f" .c)
  gcc -c -O2 -std=gnu11 -DNDEBUG -DGB_INTERNAL -D_GNU_SOURCE \
      -DGB_VERSION='"yanes-oracle"' -I"$src" -o "$obj/$name.o" "$f"
done

gcc -O2 -std=gnu11 -I"$src" -o "$out" "$root/tools/yanes_gb_oracle.c" "$obj"/*.o -lm
echo "built $out"
