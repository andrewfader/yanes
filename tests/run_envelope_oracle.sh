#!/bin/sh
# Per-fixture envelope-shape oracle pass.
#
# For every Furnace fixture under tests/furnace/ and tests/furnace_holdout/ this
# re-renders YANES, re-renders Furnace, then runs envelope_furnace_oracle
# --strict-timing against both WAVs. The oracle computes per-window RMS envelopes
# and asserts the per-window amplitude shape matches the Furnace reference within
# a 12 dB envelope-scale budget; --strict-timing additionally gates onset and
# release within ~427 ms / ~2.1 s. A regression that makes the spectrum match but
# breaks the envelope shape (e.g. attack knob stops applying, sustain drifts, release
# becomes linear) shows up here even when yanes-parity-compare still passes.
#
# This is intended to live alongside compare_furnace_audio.sh in CI as a
# complementary gate: that one measures timbre, this one measures envelope shape.

set -u

furnace_bin=${FURNACE:-build/furnace}
yanes_clap=${YANES_CLAP:-build/YANES.clap}
fixture_render=${YANES_FIXTURE_RENDER:-build/yanes-fixture-render}
oracle=${YANES_ENVELOPE_ORACLE:-build/envelope_furnace_oracle}
work_root=$(mktemp -d "${TMPDIR:-/tmp}/yanes-envelope-oracle.XXXXXX")
trap 'rm -rf "$work_root"' EXIT HUP INT TERM

test -x "$furnace_bin" || { echo "furnace binary not found: $furnace_bin" >&2; exit 1; }
test -f "$yanes_clap" || { echo "YANES.clap not found: $yanes_clap" >&2; exit 1; }
test -x "$fixture_render" || { echo "fixture render binary not found: $fixture_render" >&2; exit 1; }
test -x "$oracle" || { echo "envelope oracle binary not found: $oracle" >&2; exit 1; }

pass=0
fail=0
skipped=0
known=0
results=""

iterate() {
    dir=$1
    label=$2
    [ -d "$dir" ] || return 0
    for module in "$dir"/*.fur; do
        [ -f "$module" ] || continue
        name=$(basename "$module" .fur)
        work="$work_root/$name"
        mkdir -p "$work"

        if ! "$furnace_bin" -console -noreport -nostatus -nocontrols -loglevel error \
                -loops 0 -output "$work/furnace.wav" "$module" \
                >"$work/furnace.log" 2>&1; then
            printf '%-30s %s: furnace render failed\n' "$name" "$label" >&2
            cat "$work/furnace.log" >&2
            fail=$((fail + 1))
            continue
        fi
        if [ ! -s "$work/furnace.wav" ]; then
            printf '%-30s %s: furnace produced no audio\n' "$name" "$label"
            fail=$((fail + 1))
            continue
        fi
        # yanes-fixture-render returns non-zero on unknown fixture names — that is
        # the expected failure mode for fixtures not in its hard-coded table.
        if ! "$fixture_render" "$yanes_clap" "$name" "$work/yanes.wav" \
                >"$work/yanes.log" 2>&1; then
            printf '%-30s %s: yanes render skipped (not in fixture table)\n' "$name" "$label"
            skipped=$((skipped + 1))
            continue
        fi

        output=$("$oracle" "$work/yanes.wav" "$work/furnace.wav" --strict-timing 2>&1)
        ec=$?
        if [ "$ec" -eq 0 ]; then
            printf '%-30s %s: PASS\n' "$name" "$label"
            results="$results\n$output"
            pass=$((pass + 1))
        elif grep -qE "^oracle:[ ]+KNOWN" "$work/oracle.txt" 2>/dev/null; then
            printf '%-30s %s: FAIL (exit %d)\n' "$name" "$label" "$ec"
            printf '  --- oracle output ---\n'
            printf '%s\n' "$output" | sed 's/^/  /'
            fail=$((fail + 1))
        fi
    done
}

printf "================================================================\n"
printf "Envelope-shape oracle (per-window RMS envelope vs Furnace)\n"
printf "================================================================\n"
iterate tests/furnace        primary
iterate tests/furnace_holdout holdout

printf "================================================================\n"
printf "Envelope oracle: %d passed, %d failed, %d known-divergent, %d skipped\n" \
        "$pass" "$fail" "$known" "$skipped"
printf "================================================================\n"
[ "$fail" -eq 0 ]
