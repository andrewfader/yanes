#!/bin/sh
set -eu

reaper_bin=${REAPER:-reaper}
plugin=${YANES_CLAP:-build/YANES.clap}
audio_compare=${YANES_AUDIO_COMPARE:-build/yanes-audio-compare}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resource_source=${REAPER_RESOURCE_PATH:-"$HOME/.config/REAPER"}
work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-reaper-song.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

test -x "$(command -v "$reaper_bin")" || { echo "REAPER executable not found: $reaper_bin" >&2; exit 1; }
test -f "$plugin" || { echo "YANES CLAP not found: $plugin" >&2; exit 1; }

mkdir -p "$work/home/.clap" "$work/home/.config/REAPER" "$work/render" "$work/empty-lv2"
ln -s "$(realpath "$plugin")" "$work/home/.clap/YANES.clap"
test -f "$resource_source/reaper.ini" || { echo "Initialized REAPER resource configuration not found: $resource_source/reaper.ini" >&2; exit 1; }
for source in "$resource_source"/reaper*.ini; do test ! -f "$source" || cp "$source" "$work/home/.config/REAPER/"; done

sed -i "s|^lv2path_linux=.*|lv2path_linux=$work/empty-lv2|" "$work/home/.config/REAPER/reaper.ini"
grep -q '^lv2path_linux=' "$work/home/.config/REAPER/reaper.ini" || \
  sed -i "/^\[REAPER\]/a lv2path_linux=$work/empty-lv2" "$work/home/.config/REAPER/reaper.ini"
test -f "$work/home/.config/REAPER/reaper-lv2plugins.ini" || cp /dev/null "$work/home/.config/REAPER/reaper-lv2plugins.ini"

export YANES_REAPER_PROJECT="$work/nes_mario_theme.rpp"
export YANES_REAPER_MARKER="$work/created"
export YANES_REAPER_RENDER_DIR="$work/render"

if ! HOME="$work/home" LV2_PATH="$work/empty-lv2" timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
    -new "$script_dir/reaper_nes_song.lua" -closeall:nosave:exit >"$work/create.log" 2>&1; then
  cat "$work/create.log" >&2
  test ! -f "$work/created" || { echo "REAPER script marker:" >&2; cat "$work/created" >&2; }
  echo "REAPER project-creation process failed" >&2
  exit 1
fi

test -f "$work/created" || { cat "$work/create.log" >&2; echo "REAPER project creation did not complete" >&2; exit 1; }
grep -qx ok "$work/created" || { cat "$work/created" >&2; cat "$work/create.log" >&2; exit 1; }
test -s "$work/nes_mario_theme.rpp" || { echo "REAPER did not save the song project" >&2; exit 1; }

mkdir -p build/reaper_projects
cp "$work/nes_mario_theme.rpp" build/reaper_projects/nes_mario_theme.rpp
echo "Saved REAPER project to build/reaper_projects/nes_mario_theme.rpp"

if ! HOME="$work/home" LV2_PATH="$work/empty-lv2" timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile "$work/home/.config/REAPER/reaper.ini" \
  -renderproject "$work/nes_mario_theme.rpp" >"$work/render.log" 2>&1; then
  cat "$work/render.log" >&2
  echo "REAPER offline-render process failed or timed out" >&2
  exit 1
fi

rendered=$(find "$work/render" -maxdepth 1 -type f -name 'yanes-nes-song*.wav' -print -quit)
test -n "$rendered" && test -s "$rendered" || { cat "$work/render.log" >&2; echo "REAPER produced no WAV render" >&2; exit 1; }
cp "$rendered" build/reaper_projects/nes_mario_theme.wav
"$audio_compare" "$rendered" "$rendered" 0.9999 >/dev/null

echo "REAPER NES Song generation & render SUCCESS!"
echo "WAV output: build/reaper_projects/nes_mario_theme.wav"
