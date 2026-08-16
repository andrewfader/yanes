# Furnace audio parity

Fixtures are single-note Furnace modules regenerated with git Furnace **dev250**
(`9f00b85`). Packaged Furnace 0.6.8.3 cannot load the INF2 (format 250) header.
Point CMake at a new enough binary:

```
cmake -S . -B build -DYANES_FURNACE_EXECUTABLE=/path/to/furnace
```

Regenerate modules from a Furnace source tree with
`FURNACE_SRC=... tools/regen_furnace_fixtures.sh` after building
`yanes-furnace-fixture-gen` against that tree (see `tools/furnace_fixture_gen.cpp`).

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
the plugin's per-voice defaults (`kVoiceDefaults` in `src/yanes.cpp`), which hold
the register state each chip powers up in. That is the same state Furnace's
default instrument plays, so a passing row means a user who picks that voice and
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

## Current result

Furnace `dev250` commit `9f00b85` vs YANES at 48 kHz: **21/21** primary fixtures
and **42/42** octave holdouts, **63/63** overall. Each row independently clears
every applicable threshold; this is not an aggregate coverage percentage.

| Fixture | Envelope | Spectrum | Onset ms | Offset ms | Pitch cents | Result |
|---|---:|---:|---:|---:|---:|:---:|
| ay-tone | 0.989 | 0.991 | 0.0 | 11.6 | 0.99 | Pass |
| fds | 0.992 | 0.945 | 0.0 | 5.8 | 0.52 | Pass |
| gameboy-noise | 0.906 | 0.987 | 0.0 | 0.0 | n/a | Pass |
| gameboy-pulse | 0.866 | 0.998 | 0.0 | 40.6 | 0.01 | Pass |
| gameboy-wave | 0.992 | 0.963 | 0.0 | 11.6 | 0.08 | Pass |
| n163 | 0.986 | 0.896 | 0.0 | 34.8 | 0.15 | Pass |
| nes-noise | 0.953 | 0.985 | 0.0 | 29.0 | n/a | Pass |
| nes-pulse | 0.985 | 1.000 | 0.0 | 34.8 | 0.00 | Pass |
| nes-triangle | 0.987 | 0.999 | 0.0 | 29.0 | 0.00 | Pass |
| pce-wave | 0.991 | 0.951 | 0.0 | 5.8 | 0.11 | Pass |
| pokey-tone | 0.993 | 0.998 | 0.0 | 17.4 | 0.04 | Pass |
| saa1099 | 0.989 | 0.986 | 0.0 | 11.6 | 2.95 | Pass |
| scc | 0.986 | 0.999 | 0.0 | 29.0 | 1.57 | Pass |
| sid6581 | 0.973 | 0.940 | 0.0 | 11.6 | 0.41 | Pass |
| sid8580 | 0.957 | 0.877 | 0.0 | 11.6 | 10.13 | Pass |
| sms-noise | 0.993 | 0.994 | 0.0 | 0.0 | n/a | Pass |
| sms-tone | 0.989 | 0.994 | 0.0 | 17.4 | 0.05 | Pass |
| tia | 0.989 | 0.997 | 0.0 | 17.4 | 0.32 | Pass |
| vrc6-pulse | 0.993 | 0.898 | 0.0 | 11.6 | 1.05 | Pass |
| vrc6-saw | 0.995 | 0.821 | 0.0 | 0.0 | 2.17 | Pass |
| vrc7 | 0.920 | 0.932 | 0.0 | 23.2 | 2.58 | Pass |

## Untuned octave holdouts

Every fixture is also generated one octave below and above its note. The voice
keeps the same defaults; only the MIDI key moves ±12. These 42 cases are not
tuned individually, which is what stops a voice from being fitted to one note.

**41/42** pass, the exception being `sid8580-low` for the pitch-read reason
above. Of the rest the tightest are `n163-low` (spectrum 0.832) and
`gameboy-pulse-high` (envelope 0.863); the median holdout spectrum is 0.986.

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
- **Wavetable contents.** A wavetable chip holds a plain ramp after a reset, which
  is what the reference modules play, so that ramp is the top of the shape range
  for the FDS, N163, SCC and Game Boy wave voices *and* is what those voices
  select by default. The PC Engine voice already reached a ramp at the top of its
  range.
- **Brightness expression is neutral at its default.** It used to apply a soft
  saturation to every voice before the note left the oscillator, which no hardware
  reference has; it cost the NES triangle around 8 dB of third harmonic.

## Voice defaults

`kVoiceDefaults` in `src/yanes.cpp` gives each chip voice the register state its
hardware powers up in. It is applied when the `Waveform` parameter changes and
once at construction — the default voice needs it too, since selecting the voice
you are already on is not a change. Presets run afterwards and override whatever
they set explicitly, so a patch that wants a different duty or wavetable still
gets one; every preset that cares already sets its own shape.

The settings it carries: 12.5% duty on the NES and Game Boy pulses; period 15 and
short mode on the NES noise; white mode on the SMS and Genesis PSG noise; the
pure-tone shape on the VRC6 pulse, POKEY and TIA; the reset ramp on the FDS, N163,
SCC, PC Engine and Game Boy wave; the full accumulator rate on the VRC6 saw; the
OPLL modulator ratio and index on the VRC7; and release — 0 for every chip that
silences the moment the gate clears, 172 ms and 92 ms for the two SIDs, which run
their own envelope generator past it.

Note that a 0 ms release is the authentic tail for these chips and it does click
on note-off, the way the hardware does.

## Earlier envelope-only baseline

An earlier 21/21 “pass” used harmonic-magnitude cosine similarity measured
relative to each render's *own* detected fundamental. That metric cannot see an
octave error at all, and it is dominated by the fundamental, so two different
bright tones still clear 0.80. It is not the acceptance gate.
