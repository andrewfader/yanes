# Furnace audio parity

Fixtures are single-note Furnace modules (INF2, format 250+; packaged Furnace
0.6.8.3 cannot load them). CI builds Furnace headless at the commit pinned as
`FURNACE_REF` in `.github/workflows/ci.yml` (currently `fa9541d`) and caches the
binary per commit. Point a local build at any git Furnace binary:

```
cmake -S . -B build -DYANES_FURNACE_EXECUTABLE=/path/to/furnace
```

Without it the suite is not configured (CMake says so); a path that does not
exist is a configure error. To move to a newer Furnace, bump `FURNACE_REF`, run
the suite locally against that commit, and regenerate the modules with
`FURNACE_SRC=... tools/regen_furnace_fixtures.sh` only if chip models changed
(build `yanes-furnace-fixture-gen` against that tree first; see
`tools/furnace_fixture_gen.cpp`). Current Furnace sources need
`-DCMAKE_CXX_FLAGS="-include climits -include cstring"` with recent libstdc++.

Every fixture uses Furnace note 108. Furnace applies its own per-chip octave
convention on top of the note, and note 108 is the one that lands on the key each
fixture's entry in `tests/furnace_render.cpp` plays — C-5 for the NES pulse, C-3
for the Game Boy wave, C-6 for the SMS tone. Hand-tuning the note per chip is how
this suite previously ended up comparing renders an octave apart.

## What this suite is for

It checks that **the plugin's own defaults sound like the chip**. A fixture is
allowed to do exactly two things: select a voice with the `Waveform` parameter,
and play the note the reference module plays. Everything that shapes the sound —
duty, wavetable, noise period and mode, FM ratio and index, release — comes from
the plugin's per-voice defaults (`kVoiceDefaults` in `src/yanes.cpp`), which encode
the fixture instruments’ starting settings. These are test and musical defaults, not a guarantee
about silicon power-on state. They match what the corresponding Furnace reference instrument plays, so a passing row means a user who picks that voice and
presses a key hears the chip.

This matters because the alternative is worthless: if the harness is allowed to
dial in the settings that happen to match, the suite proves only that the engine
*can* make the sound, not that the plugin *does*. Any value the fixtures need
belongs in the voice defaults, not in `tests/furnace_render.cpp`.

## Fair composite gate

The comparator onset-aligns the two renders (renderer latency is not a timbre
failure), then requires:

- envelope correlation at least 0.80 after alignment (scale-invariant; flat
  sustains are not treated as a shape mismatch, and for noise fixtures the
  contour is smoothed before correlating so that two independent LFSR
  realisations are not scored on their block-to-block jitter);
- log-frequency band correlation at least 0.80 (24 bands, 40 Hz–16 kHz, averaged
  STFT — phase-invariant and not dominated by the fundamental). Each spectrum is
  floored 35 dB under its own strongest band, so bands holding nothing but a
  renderer's noise floor cannot outvote the harmonics;
- onset and offset within 50 ms after alignment;
- tonal pitch within 20 cents after octave folding (YIN, low-passed first so that
  wide-band chip artefacts do not drag the estimate off the true period).
  Independent chip cores do not share oscillator phase, so waveform correlation
  is not a gate. Noise fixtures omit pitch.

`yanes-parity-compare --self-test` locks the fairness rules, and runs as its own
CTest: a delayed, phase-shifted or louder copy of the same note passes, as does a
noise burst from a different LFSR seed; a square vs sine, a 50-cent sharp, an
80 ms shorter note, and a noise burst whose sustain decays away all fail.

## Envelope gate

The composite gate's envelope term is a linear correlation, which is lenient in
exactly the way that matters for attack, decay and release: a flat sustain
scores 0.866 against a note that decays 28 dB. Every fixture is therefore also
scored by `envelope_oracle` (`tests/envelope_oracle.cpp`) on the same pair of
renders:

- both sides pass through the same 15 Hz AC-coupling stage (Furnace models the
  consoles' output coupling; a DC-coupled render would otherwise differ on every
  hard note-off) and are reduced to a 5 ms-hop RMS contour in dB;
- the contours are aligned by the best lag within ±60 ms and compared up to one
  constant gain (the median offset), so chip mixing levels and output-coupling
  transients do not count as shape;
- mean |dB difference| ≤ 3 over the note (floor -40 dB), the worst 50 ms ≤ 6 dB
  over the audible envelope (floor -25 dB), and the note end (last window above
  -30 dB) within 60 ms. Noise fixtures use an 80 ms window, since an LFSR can hold
  one value for tens of milliseconds.

`envelope_oracle --self-test` runs as a CTest and plants the defects the gate
exists for: a missing attack (22 dB local), a 3x slow attack (8.5 dB), a missing
decay, a 1.5x release (9.6 dB), an ignored release and a note that never ends
all fail, while a quieter copy, engine latency, another sample rate, another
noise seed and an output-coupling onset spike all pass.

The fixtures play the reference articulation exactly: every module lifts the key
at row 16, which is 1.600 s (150 blocks) at Furnace's default speed; the SIDs
gate for one block because their default D=8/S=0 envelope has decayed long
before that.

## Current result

Furnace `fa9541d` vs YANES at 48 kHz: **21/21** primary fixtures and **42/42**
octave holdouts, **63/63** overall, each row clearing every applicable threshold of
both gates.

| Fixture | Envelope | Spectrum | Onset ms | Offset ms | Pitch cents | Env. shape dB | Env. local dB | Result |
|---|---:|---:|---:|---:|---:|---:|---:|:---:|
| ay-tone | 1.000 | 0.998 | 0.0 | 23.2 | 0.99 | 0.18 | 1.30 | Pass |
| fds | 0.999 | 0.999 | 0.0 | 29.0 | 0.14 | 0.29 | 1.15 | Pass |
| gameboy-noise | 0.999 | 0.997 | 0.0 | 29.0 | n/a | 0.11 | 0.21 | Pass |
| gameboy-pulse | 0.999 | 0.999 | 0.0 | 34.8 | 0.93 | 0.13 | 0.22 | Pass |
| gameboy-wave | 0.999 | 0.997 | 0.0 | 23.2 | 0.14 | 0.31 | 1.41 | Pass |
| n163 | 1.000 | 0.897 | 0.0 | 0.0 | 0.22 | 0.08 | 1.00 | Pass |
| nes-noise | 0.940 | 0.961 | 0.0 | 0.0 | n/a | 0.75 | 2.46 | Pass |
| nes-pulse | 1.000 | 1.000 | 0.0 | 5.8 | 0.02 | 0.04 | 0.39 | Pass |
| nes-triangle | 1.000 | 1.000 | 0.0 | 0.0 | 0.05 | 0.08 | 1.09 | Pass |
| pce-wave | 0.999 | 0.982 | 0.0 | 29.0 | 0.05 | 0.24 | 1.15 | Pass |
| pokey-tone | 1.000 | 0.998 | 0.0 | 0.0 | 0.00 | 0.03 | 0.74 | Pass |
| saa1099 | 1.000 | 0.995 | 0.0 | 23.2 | 2.86 | 0.19 | 1.38 | Pass |
| scc | 1.000 | 0.999 | 0.0 | 0.0 | 1.53 | 0.02 | 0.69 | Pass |
| sid6581 | 0.992 | 0.956 | 0.0 | 23.2 | 0.31 | 0.55 | 0.33 | Pass |
| sid8580 | 0.981 | 0.997 | 0.0 | 34.8 | 1.71 | 1.17 | 0.37 | Pass |
| sms-noise | 0.999 | 0.990 | 0.0 | 29.0 | n/a | 0.37 | 1.89 | Pass |
| sms-tone | 1.000 | 0.999 | 0.0 | 17.4 | 0.01 | 0.19 | 1.28 | Pass |
| tia | 1.000 | 0.999 | 0.0 | 17.4 | 0.24 | 0.18 | 1.36 | Pass |
| vrc6-pulse | 0.994 | 0.995 | 0.0 | 23.2 | 1.79 | 0.12 | 1.47 | Pass |
| vrc6-saw | 0.997 | 0.998 | 0.0 | 34.8 | 2.49 | 0.23 | 1.54 | Pass |
| vrc7 | 0.998 | 0.932 | 0.0 | 11.6 | 2.58 | 0.23 | 0.80 | Pass |

## Untuned octave holdouts

Every fixture is also generated one octave below and above its note. The voice
keeps the same defaults; only the MIDI key moves ±12. These 42 cases are not
tuned individually, which is what stops a voice from being fitted to one note.

**42/42 pass** both gates. The tightest are `nes-noise-low` (spectrum 0.831) and
`sid8580-low` (envelope correlation 0.972); the median holdout spectrum is 0.997;
the worst envelope-oracle rows are 1.29 dB mean and 4.36 dB local.

## What the chip models had to get right

The gate above is only meaningful because the two sides are set up to play the
same thing. Several fixtures were failing on setup or on a genuine hardware
detail rather than on anything subtle:

- **Noise pitch mapping.** The NES noise channel steps one period-table entry per
  semitone and wraps every sixteen; the Game Boy's NR43 runs four steps to the
  octave; the SN76489's noise channel clocks its shift register from the third
  tone generator's period. All three are in `src/dsp.hpp`.
- **The NES triangle's DAC.** The 2A03 sums triangle, noise and DPCM through one
  non-linear DAC. A two-level channel survives it unchanged apart from scale, but
  the triangle's staircase is bent, and that bend is the entire source of the
  even harmonics in a real NES triangle.
- **The TIA below C-4.** Its 5-bit divider runs out and the chip falls back on the
  divide-by-31 mode, whose pulse is high for 18 of its 31 counts rather than
  square — which is why a low TIA note has even harmonics and a high one does not.
- **The FDS output filter.** The chip runs its DAC through an RC low-pass around
  2 kHz. Without it the wavetable is right and the timbre is still far too bright.
- **The VRC6 saw accumulator.** Seven held levels per cycle, 8-bit accumulator,
  top five bits to the DAC — so a high rate overflows part-way through and folds
  the ramp instead of producing a clean saw.
- **Wavetable contents.** The reference modules use plain ramps, which
  the default voices therefore select, so that ramp is the top of the shape range
  for the FDS, N163, SCC and Game Boy wave voices *and* is what those voices
  select by default. The PC Engine voice already reached a ramp at the top of its
  range.
- **Brightness expression is neutral at its default.** It used to apply a soft
  saturation to every voice before the note left the oscillator, which no hardware
  reference has; it cost the NES triangle around 8 dB of third harmonic.

## Voice defaults

`kVoiceDefaults` in `src/yanes.cpp` gives each chip voice its
reference instrument’s starting settings. It is applied when the `Waveform` parameter changes and
once at construction — the default voice needs it too, since selecting the voice
you are already on is not a change. Presets run afterwards and override whatever
they set explicitly, so a patch that wants a different duty or wavetable still
gets one; every preset that cares already sets its own shape.

The settings it carries: 12.5% duty on the NES and Game Boy pulses; period 15 and
short mode on the NES noise; white mode on the SMS and Genesis PSG noise; the
pure-tone shape on the VRC6 pulse, POKEY and TIA; the reference ramp on the FDS, N163,
SCC, PC Engine and Game Boy wave; the full accumulator rate on the VRC6 saw; the
OPLL modulator ratio and index on the VRC7; the plain saw on both SIDs; the
hardware envelope (volume 15 stepping down every ~30 ms) on the Game Boy pulse and
noise, and explicitly off on every other voice; and release: 0 for every chip that
silences the moment the gate clears, 600 ms for the VRC7 and 380/315 ms for the SID
6581/8580, whose envelope generators run past it.

VRC7 and the SIDs release exponentially (the Release knob is the time to fall
60 dB), and the VRC7 carrier keeps decaying while the key is held at the FM
sustain rate (default 5, ~4.4 dB/s; 0 holds). The other chips' Release knob is a
synth convenience and fades linearly.

The channel stacks take their chips' solo defaults for the settings their channels
share (12.5% pulses, white PSG noise, pure POKEY and TIA tones, the reset ramp on the
PC Engine and SCC), so a stack's channel sounds like the solo voice of the same chip.
The six-operator, Porta FM and tine voices decay while held at the FM sustain rate,
like the VRC7, and a hardware FM chip (YM2612, OPN/OPNA, OPL, OPM) releases through
its own operator envelopes (FM release) rather than the voice Release.

A 0 ms release is the authentic tail for the gate-cut chips and it does click on
note-off, the way the hardware does.

## What the envelope gate found (September 2026)

With the linear-correlation gate alone, every row passed while several default
voices did not sound like their reference instrument. Fixed in the engine:

- **Game Boy pulse and noise sustained forever.** Furnace's default instrument
  decays over ~0.5 s on the hardware envelope; the fixtures had shortened the gate
  to 44/41 blocks to hide it.
- **VRC7 held flat and cut dead.** It now decays while held and releases
  exponentially; the fixture had stretched the gate to 165 blocks, and the engine
  carried a `key < 60` release hack for the low octave.
- **The SIDs released linearly and played saw-AND-pulse.** The reference is a
  plain saw decaying exponentially; that also lifted the 8580's spectrum from
  0.877 to 0.997.
- **The C64 output was DC-coupled.** Combined waveforms' DC offset, scaled by the
  envelope, came out as a sub-bass thump; the SID path now AC-couples at 16 Hz like
  the C64's output stage.
- **The output DC blocker clicked.** It was reset to zero whenever a sample was
  exactly 0, both at voice end (a step) and mid-note on every zero crossing of a
  custom table; it now fades its residual only once no voice is sounding.

Presets voiced before these defaults pin what they relied on (see the comment on
the preset table in `src/yanes.cpp`): the Game Boy and VRC7 recipes that sustained
still sustain, and the SID recipes keep their combined waveforms. Two changes are
audible on purpose: SID and VRC7 preset tails now fall exponentially (their release
times were scaled 1.5x so the tail reaches -40 dB when it used to), and the SID
presets lost the DC thump, so "SID 8580 lead", whose level had been measured on
that DC, was re-leveled (+8.5 dB voice gain) to stay in the preset loudness band.

## Earlier envelope-only baseline

An earlier 21/21 “pass” used harmonic-magnitude cosine similarity measured
relative to each render's *own* detected fundamental. That metric cannot see an
octave error at all, and it is dominated by the fundamental, so two different
bright tones still clear 0.80. It is not the acceptance gate.

## September 2026 audit additions

The 63 Furnace cases still cover 21 specific default voices, not every shape, FM routing,
source, or preset. Their pitch term folds octaves, so a passing row does not independently
prove absolute pitch. The new `hardware_fm_tests` checks absolute 440 Hz and 3520 Hz output
for all seven hardware-FM core configurations, and rejects an unwanted sub-octave. It also
checks that OPM AM/PM controls affect audio. These checks caught the former YM2612/OPNA
octave error and OPM semitone error that the earlier suites did not exercise.

Register-render smoke tests now use chip-specific OPL fixtures and require non-silent audio.
The comparator rejects silent tonal/noise fixtures and silent `rom-mix` excerpts; silent `rom`
isolated channels are still allowed. The generated-score NES gate has a wrong-oscillator
negative control and no longer requires a private ROM directory. It compares two local
models, so it is not a substitute for the optional emulator-based ROM oracle.
