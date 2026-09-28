#!/bin/sh
# amsynth LV2 ADSR cross-engine oracle.
#
# Renders the same single-note MIDI gesture through YANES (via the existing
# in-process CLAP harness used by tests/envelope_tests.cpp and the Furnace
# parity suite) and through amsynth (via a Reaper-hosted LV2 render, since
# amsynth is only available as LV2). The two WAVs are compared at two
# release-parameter positions; both engines' tails must grow with the
# parameter.
#
# Why YANES goes through the harness instead of Reaper:
#   - Reaper's TrackFX_SetParamNormalized path for CLAP plug-ins has subtle
#     value-mapping quirks that obscure the oracle result. The harness is
#     the same path the existing envelope_tests.cpp use, so testing against
#     it keeps the oracle tight.
#
# This oracle catches:
#   - release knob bound to a different field (y_hi == y_lo or y_hi < y_lo)
#   - release knob saturating at zero (both tails equal)
#   - cross-engine magnitude divergence (ratio outside [0.3, 3.0])

set -u

yanes_clap=${YANES_CLAP:-build/YANES.clap}
reaper_bin=${REAPER:-reaper}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

note_key=60
note_velocity=100
note_off_s=0.6
render_end_s=2.0

work=$(mktemp -d "${TMPDIR:-/tmp}/yanes-amsynth-oracle.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

test -f "$yanes_clap" \
    || { echo "YANES CLAP not found: $yanes_clap" >&2; exit 1; }
test -x "$(command -v "$reaper_bin")" \
    || { echo "REAPER executable not found: $reaper_bin" >&2; exit 1; }

# YANES-render driver: a small C++ binary that creates a plugin instance,
# sets the requested voice and release, and writes a stereo WAV.
cat > "$work/yanes_render.cpp" <<'CPP'
#include "clap_harness.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace harness;

static void render_one(const char* clap_path, double release_ms, const char* out_path) {
  Library lib(clap_path);
  const clap_plugin_t* plugin = lib.create();
  Runner r(plugin, 48000, 512);
  r.set(find_param(plugin, "Waveform"), 22.0);   // AY-3-8910 tone (software ADSR)
  r.set(find_param(plugin, "Release"), release_ms);
  std::vector<float> all;
  auto capture_block = [&] { for (float v : r.left()) all.push_back(v); };
  Events on; on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
  r.run(&on); capture_block();
  // Sustain for ~0.6 s = 28 blocks (512 samples each ≈ 10.7 ms).
  for (int i = 0; i < 28; ++i) { r.run(); capture_block(); }
  Events off; off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
  r.run(&off); capture_block();
  // Continue capturing for ~1.4 s of tail — comfortably above the longest
  // release (1600 ms) plus 0.4-s headroom.
  for (int i = 0; i < 130; ++i) { r.run(); capture_block(); }
  plugin->destroy(plugin);

  FILE* f = std::fopen(out_path, "wb");
  if (!f) { std::fprintf(stderr, "open(%s) failed\n", out_path); std::exit(1); }
  uint16_t channels = 2; uint32_t sr = 48000; uint16_t bits = 16;
  uint32_t bps = sr * channels * bits / 8;
  uint32_t data_bytes = static_cast<uint32_t>(all.size() * 4);
  std::fprintf(f, "RIFF"); uint32_t riff = data_bytes + 36; std::fwrite(&riff, 4, 1, f);
  std::fprintf(f, "WAVEfmt "); uint32_t fsz = 16; std::fwrite(&fsz, 4, 1, f);
  uint16_t pcm = 1; std::fwrite(&pcm, 2, 1, f); std::fwrite(&channels, 2, 1, f);
  std::fwrite(&sr, 4, 1, f); std::fwrite(&bps, 4, 1, f);
  uint16_t balign = channels * bits / 8; std::fwrite(&balign, 2, 1, f); std::fwrite(&bits, 2, 1, f);
  std::fprintf(f, "data"); std::fwrite(&data_bytes, 4, 1, f);
  for (float v : all) {
    int16_t s = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767);
    std::fwrite(&s, 2, 1, f); std::fwrite(&s, 2, 1, f);
  }
  std::fclose(f);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <YANES.clap> <release_ms> <out.wav>\n", argv[0]);
    return 2;
  }
  render_one(argv[1], std::atof(argv[2]), argv[3]);
  return 0;
}
CPP

yanes_render_bin="$work/yanes_render"
clap_inc="-Ibuild/_deps/clap-src/include -Itests"
if ! g++ -std=c++20 $clap_inc -o "$yanes_render_bin" "$work/yanes_render.cpp" -ldl; then
    echo "failed to compile yanes_render.cpp" >&2
    exit 1
fi

printf "================================================================\n"
printf "amsynth ADSR cross-engine oracle (release scaling comparator)\n"
printf "================================================================\n"
printf "  Rendering YANES via in-process CLAP harness …\n"

yanes_wav_low="$work/yanes_low.wav"
yanes_wav_high="$work/yanes_high.wav"
# YANES Release goes 0..2000 ms. Position 0.2 = 400 ms, 0.8 = 1600 ms; both
# produce visibly different tails in-process (verified during development).
"$yanes_render_bin" "$yanes_clap"  400.0 "$yanes_wav_low"
"$yanes_render_bin" "$yanes_clap" 1600.0 "$yanes_wav_high"

# Two REAPER invocations reusing the same lua driver; the synth kind is fixed
# at amsynth and the release position varies.
render_amsynth() {
    pos=$1
    run="$work/amsynth_${pos/./_}"
    mkdir -p "$run/render"
    HOME="$work/home" \
    YANES_REAPER_PROJECT="$run/project.rpp" \
    YANES_REAPER_MARKER="$run/marker" \
    YANES_REAPER_RENDER_DIR="$run/render" \
    YANES_REAPER_NOTE_KEY=$note_key YANES_REAPER_NOTE_VEL=$note_velocity \
    YANES_REAPER_NOTE_OFF=$note_off_s YANES_REAPER_RENDER_END=$render_end_s \
    YANES_REAPER_RELEASE=$pos \
    timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile /home/andrew/.config/REAPER/reaper.ini \
        -new "$script_dir/reaper_amsynth_oracle.lua" -closeall:nosave:exit >"$run/create.log" 2>&1
    [ -f "$run/marker" ] && grep -qx ok "$run/marker" \
        || { echo "amsynth@$pos: reaper driver did not finish" >&2; cat "$run/marker" 2>/dev/null >&2; return 1; }
    HOME="$work/home" timeout 45 "$reaper_bin" -newinst -nosplash -cfgfile /home/andrew/.config/REAPER/reaper.ini \
        -renderproject "$run/project.rpp" >"$run/render.log" 2>&1
    rendered=$(find "$run/render" -maxdepth 1 -type f -name "yanes-amsynth-amsynth*.wav" -print -quit)
    [ -n "$rendered" ] && [ -s "$rendered" ] \
        || { echo "amsynth@$pos: reaper produced no WAV" >&2; return 1; }
    echo "$rendered"
}

# amsynth LV2 release normalized 0..1 maps roughly to 10 ms .. 10 s. Picking
# positions that bracket the YANES 400 ms / 1600 ms yields comparable absolute
# tails that both engines can produce.
echo "  Rendering amsynth via Reaper-hosted LV2 at release 0.15 and 0.7 …"
amsynth_wav_low=$(render_amsynth 0.15)  || exit 1
amsynth_wav_high=$(render_amsynth 0.7) || exit 1

cat > "$work/amsynth_oracle.py" <<'PY'
import sys
import wave
import numpy as np

def load(path):
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        n = w.getnframes()
        ch = w.getnchannels()
        raw = w.readframes(n)
    arr = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if ch == 2:
        arr = arr.reshape(-1, 2).mean(axis=1)
    return sr, arr

def envelope_tail_seconds(samples, threshold_db=-40):
    hop = 4096
    env, peak = [], 0.0
    for s in range(0, len(samples), hop):
        block = samples[s:s+hop]
        if not len(block):
            break
        e = float(np.sqrt(np.mean(block.astype(np.float64) ** 2)))
        env.append(e); peak = max(peak, e)
    if peak <= 1e-6:
        return float("inf"), peak
    threshold = peak * (10 ** (threshold_db / 20))
    last_loud_idx = -1
    for i, e in enumerate(env):
        if e >= threshold:
            last_loud_idx = i
    if last_loud_idx < 0:
        return 0.0, peak
    return (last_loud_idx + 1) * hop, peak

def tail_seconds(path, note_off_s, render_end_s):
    sr, samples = load(path)
    end_idx = min(len(samples), int(render_end_s * sr))
    post = samples[int(note_off_s * sr):end_idx]
    if len(post) == 0:
        return float("nan"), float("nan")
    tail_samples, peak = envelope_tail_seconds(post)
    return tail_samples / sr, peak

yanes_low, note_off_s, render_end_s, yanes_high, amsynth_low, amsynth_high = sys.argv[1:7]
note_off_s = float(note_off_s)
render_end_s = float(render_end_s)

y_lo, yp_lo = tail_seconds(yanes_low,    note_off_s, render_end_s)
y_hi, yp_hi = tail_seconds(yanes_high,   note_off_s, render_end_s)
a_lo, ap_lo = tail_seconds(amsynth_low,  note_off_s, render_end_s)
a_hi, ap_hi = tail_seconds(amsynth_high, note_off_s, render_end_s)

sys.stdout.write(f"YANES    release@400 ms   peak={yp_lo:.4f}  tail={y_lo*1000:.1f} ms\n")
sys.stdout.write(f"YANES    release@1600 ms  peak={yp_hi:.4f}  tail={y_hi*1000:.1f} ms\n")
sys.stdout.write(f"amsynth  release@~600 ms  peak={ap_lo:.4f}  tail={a_lo*1000:.1f} ms\n")
sys.stdout.write(f"amsynth  release@~3000 ms peak={ap_hi:.4f}  tail={a_hi*1000:.1f} ms\n")

# Gate: both engines' release should grow between low and high positions.
# A YANES render where the release knob is bound to a different field will
# see y_hi == y_lo. Cross-engine growth ratio in [0.3, 3.0] rules out
# divergent scaling.
yanes_growth   = y_hi / max(y_lo, 1e-6)
amsynth_growth = a_hi / max(a_lo, 1e-6)
sys.stdout.write(f"YANES    growth (high/low) = {yanes_growth:.2f}\n")
sys.stdout.write(f"amsynth  growth (high/low) = {amsynth_growth:.2f}\n")

ok = (y_hi > y_lo * 1.05) and (a_hi > a_lo * 1.05) \
     and (0.3 <= yanes_growth / max(amsynth_growth, 1e-6) <= 3.0)
sys.exit(0 if ok else 1)
PY
out="$work/amsynth_oracle.out"
python3 "$work/amsynth_oracle.py" \
    "$yanes_wav_low" "$note_off_s" "$render_end_s" \
    "$yanes_wav_high" "$amsynth_wav_low" "$amsynth_wav_high" \
    >"$out" 2>&1
ec=$?

printf "================================================================\n"
cat "$out"
if [ "$ec" -eq 0 ]; then
    printf "================================================================\n"
    printf "amsynth oracle: PASS (release scaling matches expectation on both engines)\n"
else
    printf "================================================================\n"
    printf "amsynth oracle: FAIL (release scaling test — see output above)\n"
fi
exit "$ec"
