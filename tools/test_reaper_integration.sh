#!/bin/sh
set -eu

reaper_bin=${REAPER:-reaper}
plugin=${YANES_CLAP:-build/YANES.clap}
audio_compare=${YANES_AUDIO_COMPARE:-build/yanes-audio-compare}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resource_source=${REAPER_RESOURCE_PATH:-"$HOME/.config/REAPER"}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-reaper.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

test -x "$(command -v "$reaper_bin")" || { echo "REAPER executable not found: $reaper_bin" >&2; exit 1; }
test -f "$plugin" || { echo "YANES CLAP not found: $plugin" >&2; exit 1; }

mkdir -p "$work/home/.clap" "$work/home/.config/REAPER" "$work/render" "$work/empty-lv2"
ln -s "$(realpath "$plugin")" "$work/home/.clap/YANES.clap"
test -f "$resource_source/reaper.ini" || { echo "Initialized REAPER resource configuration not found: $resource_source/reaper.ini" >&2; exit 1; }
for source in "$resource_source"/reaper*.ini; do test ! -f "$source" || cp "$source" "$work/home/.config/REAPER/"; done
# Keep the host test focused on CLAP. REAPER otherwise walks every system LV2
# bundle when its disposable resource directory has no populated LV2 cache.
sed -i "s|^lv2path_linux=.*|lv2path_linux=$work/empty-lv2|" "$work/home/.config/REAPER/reaper.ini"
grep -q '^lv2path_linux=' "$work/home/.config/REAPER/reaper.ini" || \
  sed -i "/^\[REAPER\]/a lv2path_linux=$work/empty-lv2" "$work/home/.config/REAPER/reaper.ini"
# An empty cache intentionally disables an unrelated full LV2 rescan in this isolated run.
test -f "$work/home/.config/REAPER/reaper-lv2plugins.ini" || cp /dev/null "$work/home/.config/REAPER/reaper-lv2plugins.ini"

if ! YANES_REAPER_PROJECT="$work/yanes.rpp" \
  YANES_REAPER_MARKER="$work/created" \
  YANES_REAPER_RENDER_DIR="$work/render" \
  HOME="$work/home" LV2_PATH="$work/empty-lv2" timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
    -new "$script_dir/reaper_integration.lua" -closeall:nosave:exit >"$work/create.log" 2>&1; then
  cat "$work/create.log" >&2
  test ! -f "$work/created" || { echo "REAPER script marker:" >&2; cat "$work/created" >&2; }
  test ! -f "$work/yanes.rpp" || echo "REAPER saved the project before failing to exit" >&2
  echo "REAPER project-creation process failed or timed out" >&2
  exit 1
fi

test -f "$work/created" || { cat "$work/create.log" >&2; echo "REAPER project creation did not complete" >&2; exit 1; }
grep -qx ok "$work/created" || { cat "$work/created" >&2; cat "$work/create.log" >&2; exit 1; }
test -s "$work/yanes.rpp" || { echo "REAPER did not save the integration project" >&2; exit 1; }

if ! HOME="$work/home" LV2_PATH="$work/empty-lv2" timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
  -renderproject "$work/yanes.rpp" >"$work/render.log" 2>&1; then
  cat "$work/render.log" >&2
  echo "REAPER offline-render process failed or timed out" >&2
  exit 1
fi

rendered=$(find "$work/render" -maxdepth 1 -type f -name 'yanes-reaper*.wav' -print -quit)
test -n "$rendered" && test -s "$rendered" || { cat "$work/render.log" >&2; echo "REAPER produced no WAV render" >&2; exit 1; }
"$audio_compare" "$rendered" "$rendered" 0.9999 >/dev/null
# The render must carry what the project says: twelve kit hits in the first 0.7 s (the preset
# recalled from the saved project), a held C-4 from 0.82 s that is pitched and sustained (the
# automation moved the source to Porta FM before it), and silence once the note has released.
python3 - "$rendered" <<'PY' || { echo "REAPER render does not match the project it was rendered from" >&2; exit 1; }
import math, struct, sys, wave
with wave.open(sys.argv[1], 'rb') as w:
    rate, channels, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(frames)
assert width in (2, 3, 4), width
def sample(i):
    if width == 2: return struct.unpack_from('<h', raw, i * 2)[0] / 32768.0
    if width == 3: b = raw[i * 3:i * 3 + 3]; return int.from_bytes(b, 'little', signed=True) / 8388608.0
    return struct.unpack_from('<i', raw, i * 4)[0] / 2147483648.0
mono = [sum(sample(f * channels + c) for c in range(channels)) / channels for f in range(frames)]
def rms_db(a, b):
    seg = mono[int(a * rate):int(b * rate)]
    return 10 * math.log10(sum(x * x for x in seg) / max(1, len(seg)) + 1e-20)
def crossings(a, b):
    seg = mono[int(a * rate):int(b * rate)]
    return sum(1 for i in range(1, len(seg)) if seg[i - 1] < 0 <= seg[i])
hits = rms_db(0.0, 0.7); held = rms_db(0.9, 1.6); tail = rms_db(1.9, 2.0)
hz = crossings(1.0, 1.5) / 0.5
print(f"hits {hits:.1f} dB, held note {held:.1f} dB at ~{hz:.0f} Hz, tail {tail:.1f} dB")
assert hits > -40, "the drum kit passage is missing (preset not recalled)"
assert held > -40 and rms_db(1.4, 1.6) - rms_db(0.9, 1.1) > -6, "the held note is not sustained (automation to Porta FM missing)"
assert 200 < hz < 330, f"the held note is not pitched near C-4 ({hz:.0f} Hz)"
assert tail < held - 30, "the note does not release"
PY
echo "REAPER CLAP integration passed: project recall, MIDI, automation, and offline render"
