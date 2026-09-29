#!/usr/bin/env bash
# Reaper-host cross-engine envelope oracle.
#
# For each Furnace primary fixture, this script:
#   1. Renders the fixture through the Furnace CLI to build the reference WAV
#      at 44100 Hz (Furnace's native rate).
#   2. Drives Reaper to instantiate the YANES CLAP plug-in, build a one-note
#      MIDI item with the same note-key + note-off time, and offline-render the
#      result at 48000 Hz through Reaper's audio bus.
#   3. Scores the pair with yanes-parity-compare (proves the host delivered the
#      voice: timbre and pitch) and envelope_oracle (attack, decay, release).
#
# This catches:
#   - envelope bugs that only appear when the plug-in runs through a real host
#     (latency compensation, automation scheduling, MIDI event delivery)
#   - cases where the CLAP harness's render diverges from Reaper's render of
#     the same plug-in instance
#   - any new plugin build that breaks CLAP instantiation or render output
#
# This is meant to run alongside tools/test_reaper_integration.sh and the
# fixture-render comparison. CI can run it without a display.

set -u

reaper_bin=${REAPER:-reaper}
yanes_clap=${YANES_CLAP:-build/YANES.clap}
furnace_bin=${FURNACE:-build/furnace}
oracle=${YANES_ENVELOPE_ORACLE:-build/envelope_oracle}
compare=${YANES_PARITY_COMPARE:-build/yanes-parity-compare}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resource_source=${REAPER_RESOURCE_PATH:-"$HOME/.config/REAPER"}

work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-reaper-oracle.XXXXXX")
[ -n "${YANES_KEEP_WORK:-}" ] || trap 'rm -rf "$work"' EXIT HUP INT TERM

test -x "$(command -v "$reaper_bin")" \
    || { echo "REAPER executable not found: $reaper_bin" >&2; exit 1; }
test -f "$yanes_clap" \
    || { echo "YANES CLAP not found: $yanes_clap" >&2; exit 1; }
test -x "$furnace_bin" \
    || { echo "furnace binary not found: $furnace_bin" >&2; exit 1; }
test -x "$oracle" \
    || { echo "envelope oracle binary not found: $oracle" >&2; exit 1; }

mkdir -p "$work/home/.clap" "$work/home/.config/REAPER" "$work/render" "$work/empty-lv2"
ln -s "$(realpath "$yanes_clap")" "$work/home/.clap/YANES.clap"
test -f "$resource_source/reaper.ini" \
    || { echo "Initialized REAPER resource configuration not found: $resource_source/reaper.ini" >&2; exit 1; }
for source in "$resource_source"/reaper*.ini; do test ! -f "$source" || cp "$source" "$work/home/.config/REAPER/"; done
sed -i "s|^lv2path_linux=.*|lv2path_linux=$work/empty-lv2|" "$work/home/.config/REAPER/reaper.ini"
grep -q '^lv2path_linux=' "$work/home/.config/REAPER/reaper.ini" \
    || sed -i "/^\\[REAPER\\]/a lv2path_linux=$work/empty-lv2" "$work/home/.config/REAPER/reaper.ini"
test -f "$work/home/.config/REAPER/reaper-lv2plugins.ini" \
    || cp /dev/null "$work/home/.config/REAPER/reaper-lv2plugins.ini"

pass=0
fail=0

# Per-fixture orchestration. For every primary Furnace module we know exactly
# which voice the fixture renderer picks, how long it holds the note, and
# which MIDI key it plays (see tests/furnace_render.cpp). That data drives the
# Reaper-side driver so both engines render the same gesture; only the host
# path (CLAP harness vs Reaper bus) differs.
#
# Format: "<name>|<waveform_index>|<note_key>|<note_off_s>|<render_end_s>"
# Every reference module lifts the key at 1.6 s (row 16); the SIDs gate for one
# 512-frame block, as in tests/furnace_render.cpp.
oracle_fixtures() {
    cat <<'EOF'
ay-tone|22|60|1.6|3.2
fds|5|72|1.6|3.2
gameboy-noise|12|60|1.6|3.2
gameboy-pulse|10|60|1.6|3.2
gameboy-wave|11|48|1.6|3.2
n163|6|60|1.6|3.2
nes-noise|2|60|1.6|3.2
nes-pulse|0|72|1.6|3.2
nes-triangle|1|60|1.6|3.2
pce-wave|26|72|1.6|3.2
pokey-tone|24|60|1.6|3.2
sid6581|38|60|0.011|3.2
sid8580|39|60|0.011|3.2
saa1099|42|60|1.6|3.2
scc|40|72|1.6|3.2
sms-noise|14|84|1.6|3.2
sms-tone|13|84|1.6|3.2
tia|44|60|1.6|3.2
vrc6-pulse|3|72|1.6|3.2
vrc6-saw|4|72|1.6|3.2
vrc7|7|60|1.6|3.2
EOF
}

printf "================================================================\n"
printf "Reaper-host cross-engine envelope oracle\n"
printf "================================================================\n"

while IFS='|' read -r name waveform note_key note_off_s render_end_s; do
    [ -n "$name" ] || continue
    run="$work/$name"
    mkdir -p "$run"
    module="tests/furnace/${name}.fur"
    [ -f "$module" ] || { printf '%-30s FAIL (no furnace module)\n' "$name"; fail=$((fail + 1)); continue; }

    # Furnace renders the reference. Cached once for all tests so we only pay
    # the cost of loading and rendering each .fur once.
    if ! "$furnace_bin" -console -noreport -nostatus -nocontrols -loglevel error \
            -loops 0 -output "$run/furnace.wav" "$module" >"$run/furnace.log" 2>&1; then
        printf '%-30s FAIL (furnace render failed)\n' "$name"
        fail=$((fail + 1))
        continue
    fi
    if [ ! -s "$run/furnace.wav" ]; then
        printf '%-30s FAIL (furnace produced no audio)\n' "$name"
        fail=$((fail + 1))
        continue
    fi

    # Build the Reaper project via the oracle driver.
    HOME="$work/home" LV2_PATH="$work/empty-lv2" \
    YANES_REAPER_PROJECT="$run/project.rpp" \
    YANES_REAPER_MARKER="$run/marker" \
    YANES_REAPER_RENDER_DIR="$run/render" \
    YANES_REAPER_FIXTURE="$name" \
    YANES_REAPER_NOTE_KEY="$note_key" YANES_REAPER_NOTE_VEL=110 \
    YANES_REAPER_NOTE_OFF="$note_off_s" YANES_REAPER_RENDER_END="$render_end_s" \
    YANES_REAPER_WAVEFORM="$waveform" \
    timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
        -new "$script_dir/reaper_oracle_driver.lua" -closeall:nosave:exit >"$run/create.log" 2>&1 \
        || { printf '%-30s FAIL (reaper -new failed)\n' "$name"; cat "$run/create.log" >&2; fail=$((fail + 1)); continue; }
    [ -f "$run/marker" ] && grep -qx ok "$run/marker" \
        || { printf '%-30s FAIL (reaper driver did not complete)\n' "$name"; cat "$run/marker" 2>/dev/null >&2; fail=$((fail + 1)); continue; }
    [ -s "$run/project.rpp" ] \
        || { printf '%-30s FAIL (reaper project not saved)\n' "$name"; fail=$((fail + 1)); continue; }

    # Render offline.
    HOME="$work/home" LV2_PATH="$work/empty-lv2" \
    timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
        -renderproject "$run/project.rpp" >"$run/render.log" 2>&1 \
        || { printf '%-30s FAIL (reaper render failed)\n' "$name"; cat "$run/render.log" >&2; fail=$((fail + 1)); continue; }
    rendered=$(find "$run/render" -maxdepth 1 -type f -name "${name}*.wav" -print -quit)
    [ -n "$rendered" ] && [ -s "$rendered" ] \
        || { printf '%-30s FAIL (reaper did not produce a WAV)\n' "$name"; fail=$((fail + 1)); continue; }

    # Timbre and pitch first: a render of the wrong voice can share a sustained
    # envelope with the right one, so the envelope alone cannot prove the host
    # delivered the voice.
    window=20; kind=tonal
    case "$name" in *noise*) window=80; kind=noise;; esac
    if "$compare" "$run/furnace.wav" "$rendered" "$kind" 0.8 > "$run/oracle.log" 2>&1 &&
       "$oracle" "$rendered" "$run/furnace.wav" --window-ms "$window" >> "$run/oracle.log" 2>&1; then
        printf '%-30s PASS\n' "$name"
        pass=$((pass + 1))
    else
        printf '%-30s FAIL\n' "$name"
        printf '  --- oracle output ---\n'
        sed 's/^/  /' "$run/oracle.log"
        fail=$((fail + 1))
    fi
done < <(oracle_fixtures)

printf "================================================================\n"
printf "Reaper oracle: %d passed, %d failed\n" "$pass" "$fail"
printf "================================================================\n"
[ "$fail" -eq 0 ]
