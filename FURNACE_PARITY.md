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

To build the generator, append this to the Furnace checkout's `CMakeLists.txt` (it reuses
Furnace's sources minus `main.cpp`; the generator file itself needs C++17 for `extra_chips.hpp`)
and build `yanes-furnace-fixture-gen`:

```cmake
set(YANES_FIXTURE_GEN_SOURCES ${USED_SOURCES})
list(REMOVE_ITEM YANES_FIXTURE_GEN_SOURCES src/main.cpp furnace.appdata.xml)
list(APPEND YANES_FIXTURE_GEN_SOURCES /path/to/yanes/tools/furnace_fixture_gen.cpp)
add_executable(yanes-furnace-fixture-gen ${YANES_FIXTURE_GEN_SOURCES})
target_include_directories(yanes-furnace-fixture-gen SYSTEM PRIVATE ${DEPENDENCIES_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src)
target_include_directories(yanes-furnace-fixture-gen PRIVATE /path/to/yanes/src)
target_compile_options(yanes-furnace-fixture-gen PRIVATE ${DEPENDENCIES_COMPILE_OPTIONS})
target_compile_definitions(yanes-furnace-fixture-gen PRIVATE ${DEPENDENCIES_DEFINES})
target_link_libraries(yanes-furnace-fixture-gen PRIVATE ${DEPENDENCIES_LIBRARIES})
target_link_directories(yanes-furnace-fixture-gen PRIVATE ${DEPENDENCIES_LIBRARY_DIRS})
target_link_options(yanes-furnace-fixture-gen PRIVATE ${DEPENDENCIES_LINK_OPTIONS})
target_precompile_headers(yanes-furnace-fixture-gen PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h>)
set_source_files_properties(/path/to/yanes/tools/furnace_fixture_gen.cpp PROPERTIES
  COMPILE_OPTIONS "-std=gnu++17" SKIP_PRECOMPILE_HEADERS ON)
```

Regenerating an existing fixture this way gives a byte-different module that renders the
identical audio (checked on `nes-pulse`), so there is no need to rewrite committed modules when
adding new ones.

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

## Second wave (October 2026)

Sixteen more fixtures cover the chips added in October 2026, each with octave holdouts:
**110 registered parity tests pass** (37 primaries, 73 holdouts). Two cases are kept as files but
not registered; see *Known gaps* below.

Most of these fixtures play Furnace's default instrument untouched. Where the default instrument
is silent or does not use the mode that defines the YANES voice, the generator
(`tools/furnace_fixture_gen.cpp`) sets exactly what the YANES voice's own defaults select, and
nothing else:

| Fixture | Furnace system, channel | Set by the fixture | YANES voice, key |
|---|---|---|---|
| pc-speaker | PC Speaker | nothing | PC speaker, C-5 |
| zx-beeper | ZX Spectrum beeper (SFX engine) | nothing | ZX Spectrum beeper, C-4 |
| virtual-boy-wave / -noise | Virtual Boy, ch 1 / ch 6 | nothing | VSU wavetable / VB noise, C-4 |
| wonderswan-wave / -noise | WonderSwan, ch 1 / ch 4 | noise: duty macro 1 (noise on, first tap) | WonderSwan wavetable / noise, C-5 |
| lynx | Atari Lynx, ch 1 | duty macro 1 (the f0 tap set; no taps is a DC level) | Atari Lynx Mikey, C-4 (Furnace note 84) |
| msx-ym2413 | YM2413, ch 1 | ROM instrument 1 (violin), the YANES default | MSX YM2413 FM, C-4 |
| msx-bass-drum, -snare, -tom, -hihat | YM2413 drums mode, ch 7-11 | nothing | MSX-MUSIC stack rhythm channels, C-4 |
| ay-buzzer | AY-3-8910, 2 MHz clock | wave macro 4 (envelope, no tone); effects 22 81, 23/24 = round(2 MHz / (256 f)) | AY envelope buzzer, C-4 |
| amiga | Amiga | YANES's built-in 32-byte saw loop as the sample | Amiga Paula, C-4 |
| gba-minmod | GBA MinMod (software mixer), 13379 Hz | YANES's built-in 64-byte string loop | GBA DirectSound, C-4 |
| msm6295 | OKI MSM6295 | YANES's built-in arcade kick, at 7575 Hz | OKI MSM6295 ADPCM, C-4 (the kick) |

Furnace's Lynx driver clamps its timer above about D#5, so Furnace note 108 (C-6) plays 347 Hz;
the Lynx fixture is centred on C-4 instead. The AY buzzer sets the envelope period directly
because Furnace's auto-envelope truncates the tone period (period >> 4), which lands up to a third
of a semitone sharp; YANES picks the nearest period. Snare, cymbal and hi-hat are scored as noise.

| Fixture | Envelope | Spectrum | Onset ms | Offset ms | Pitch cents | Env. shape dB | Env. local dB | End ms | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| pc-speaker | 1.000 | 0.982 | 0.0 | 17.4 | 0.01 | 0.14 | 0.56 | -5 | Pass |
| zx-beeper | 0.998 | 0.997 | 0.0 | 0.0 | 5.39 | 0.11 | 0.13 | 0 | Pass |
| virtual-boy-wave | 0.998 | 1.000 | 0.0 | 23.2 | 0.04 | 0.28 | 1.06 | -20 | Pass |
| virtual-boy-noise | 1.000 | 0.999 | 0.0 | 5.8 | n/a | 0.07 | 0.28 | -10 | Pass |
| wonderswan-wave | 0.999 | 0.995 | 0.0 | 23.2 | 0.06 | 0.27 | 1.07 | -25 | Pass |
| wonderswan-noise | 1.000 | 0.991 | 0.0 | 23.2 | n/a | 0.30 | 0.58 | -25 | Pass |
| lynx | 1.000 | 0.996 | 0.0 | 23.2 | 8.08 | 0.07 | 1.31 | 0 | Pass |
| msx-ym2413 | 0.998 | 0.998 | 0.0 | 23.2 | 1.39 | 0.23 | 0.56 | -55 | Pass |
| msx-bass-drum | 1.000 | 0.998 | 0.0 | 5.8 | 7.92 | 0.31 | 0.13 | 0 | Pass |
| msx-snare | 1.000 | 0.962 | 0.0 | 0.0 | n/a | 0.14 | 0.37 | -10 | Pass |
| msx-tom | 1.000 | 1.000 | 0.0 | 0.0 | 3.29 | 0.53 | 0.21 | -5 | Pass |
| msx-hihat | 0.999 | 0.824 | 0.0 | 11.6 | n/a | 2.40 | 0.71 | -30 | Pass |
| ay-buzzer | 1.000 | 1.000 | 0.0 | 5.8 | 0.06 | 0.18 | 1.36 | -5 | Pass |
| amiga | 0.999 | 0.998 | 0.0 | 17.4 | 3.64 | 0.21 | 1.64 | -5 | Pass |
| gba-minmod | 0.996 | 0.998 | 0.0 | 11.6 | 1.54 | 0.06 | 0.40 | 0 | Pass |
| msm6295 | 1.000 | 1.000 | 0.0 | 0.0 | 3.73 | 0.09 | 0.11 | -5 | Pass |
| msx-cymbal (not registered) | 0.997 | 0.995 | 0.0 | 29.0 | n/a | 1.20 | 1.12 | -65 | Fail |

### What the references found

- **ymfm's OPL/OPLL hi-hat and cymbal phase.** Fed identical registers, ymfm's top cymbal
  differed from Nuked-OPLL (the die-derived core Furnace uses). ymfm built the phase select as
  `(hh2 ^ hh7) | hh3 | (tc3 ^ tc5)`; Nuked-OPLL and Nuked-OPL3 use `(hh2 ^ hh7) | (hh3 ^ tc5) |
  (tc3 ^ tc5)`. `third_party/ymfm-rhythm-phase-select.patch` makes that one-term change, applied
  when CMake fetches ymfm.
- **YM2413 F-numbers.** The cymbal and hi-hat partials are XORs of phase bits, so one F-number
  step changes them completely: C-4 as block 3 / 345 and as block 4 / 172 sound unrelated. MSX
  drivers and Furnace keep a one-octave table from about 172 to 343 per block; YANES now does too
  (which also brought the melodic YM2413 from 6.6 to 1.4 cents).
- **YM2413 drums follow the note**, written to the drum's own channel (BD ch 6, SD/HH ch 7, TOM/TC
  ch 8), and the drum channels nobody plays stay at F-number 0, as in Furnace. Preloading the
  MSX-BIOS values there changed the cymbal, which mixes in channel 7's phase.
- **ymfm output resampling.** Holding the latest chip sample skipped one chip sample in every 29
  at 48 kHz (the YM2413 runs at 49716 Hz); the hardware FM voices now interpolate between the last
  two chip samples.
- **ZX pin pulses.** A pin-pulse engine fires a fixed-length pulse each period (64 ticks of its
  895 kHz loop by default), not a fraction of the period; Pulse duty now picks 32/64/128/256 ticks.
- **Lynx LFSR.** Mikey's core in Furnace is the same model (`shift << 1 | !parity(taps)`), and the
  loop lengths of YANES's seven tonal tap sets (2, 4, 7, 9, 15, 31, 63) are the ones Furnace's
  `DUTY_DIVIDERS` table lists. The VSU and WonderSwan noise taps give the documented periods
  32767, 1953, 254, 217, 73, 63, 42, 28.

### Known gaps

ymfm's YM2413 envelope releases about 1.25 times faster than Furnace's Nuked-OPLL for the same
registers (the violin's carrier release: about 200 dB/s against 160; the cymbal's decay: about
52 dB/s against 42). Both ROM tables give the violin carrier release rate 7 and both cores compute
the same rate, 29, so the difference is in ymfm's envelope stepping; it has not been pinned to a
line. Two cases miss the 60 ms note-end gate because of it and are not registered (CMake prints
which): `msx-ym2413` one octave down (70 ms) and `msx-cymbal` (65 ms; its spectrum, 0.995, and
envelope shape pass). Their modules are in `tests/furnace` and `tests/furnace_holdout`.

Apple II speaker, TMS5220 speech and the slap bass have no Furnace reference (Furnace has no
Apple II or TMS5220 chip); their behaviour is proved by `tests/extra_chips_tests.cpp` and the
Waveform proof in `tests/advertised_tests.cpp`.

### What this gate does not see

The composite gate compares 24 log-spaced, phase-blind bands with a 0.8 threshold, plus pitch and
envelope. Planted defects show its reach: a broken Lynx feedback bit fails it, but a linear AY
DAC in place of the log one (spectrum 0.9996), a VSU noise tap off by one table entry (0.990),
doubled OKI ADPCM step sizes (0.9996) and a 4-bit WonderSwan table reduced to 3 bits (0.995) all
pass. Those details are pinned by the unit tests (DAC table, tap periods, the ADPCM decode rule)
and the plug-in proofs, not by this suite.

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

## SNES S-DSP parity (hardware reference, more accurate than Furnace)

The SNES sample-bank voice is a sampler, not an SPC700 song player, so it cannot join the
default-voice Furnace suite (that suite's contract is "pick a voice, play a note", and the SNES
has no default sample). Instead `snes_dsp_parity` pins the voice directly to the hardware
reference: blargg's `SPC_DSP` (the cycle-accurate S-DSP used by accurate SNES emulators, and the
same core Furnace's SNES chip builds on). The test feeds an identical BRR sample to both engines
at matched pitches and requires >0.9 correlation; in practice a synthetic sine matches at 0.9998
and real Donkey Kong Country cartridge samples at 0.988–0.9998.

Enable it by pointing at the reference sources (a Furnace checkout's
`src/engine/platform/sound/snes`, which holds `SPC_DSP.cpp`):

```sh
cmake -S . -B build -DYANES_SNES_DSP_REFERENCE=/path/to/furnace/src/engine/platform/sound/snes
```

The reference is test-only and never linked into the product. Set `YANES_SNES_SAMPLES` (a path
list of `.brr` files) to also pin the voice to real cartridge samples; it is skipped when unset.

## SMS SN76489 real-ROM parity (more accurate than Furnace)

Where a real game can be run on a hardware-accurate emulator, that beats a chip-model comparison
against Furnace. `tools/test_sms_rom_parity.sh` renders a real Master System ROM through Genesis
Plus GX, patched to log its SN76489 register writes
(`third_party/genesis-plus-gx-psg-register-log.patch`), then replays that register stream
independently through the plug-in's own PSG primitives (`tools/yanes_sms_replay.cpp`) and scores
the two audios. Pure-PSG titles (Alex Kidd in Miracle World / Shinobi World, Sonic the Hedgehog 2)
match the real game at envelope 0.8–0.999 and spectrum 0.76–0.95; FM-add-on games are excluded
because their audio is the YM2413, not the PSG. Enable with
`-DYANES_SMS_ROM=/path/to/game.sms` (the patched core defaults to
`../yanes-oracles/genesis-plus-gx/genesis_plus_gx_libretro.so`); `tools/test_real_roms.sh` runs
the SMS set alongside the NES, Game Boy and PC Engine lanes.

## SID reference parity (reSIDfp)

The SID voice is benchmarked against reSIDfp — the reference MOS6581/8580 emulation used by accurate
SID players — rather than against Furnace. `sid_resid_parity` drives the same note and waveform
through both reSIDfp and the plug-in and cross-correlates the DC-removed steady-state waveform (the
DC offset the 8580 carries is discarded by the real console's output capacitor and the plug-in's DC
blocker alike). The pure oscillator waveforms track the reference closely: 6581 saw/triangle/pulse
at 0.96/0.97/0.99 and 8580 at 0.89/0.91/0.96, all above the 0.80 gate. It auto-enables when the
system libresidfp is installed and is test-only, never linked into the product. YANES's SID is an
original musical model, not a reSID clone, so this is a closeness benchmark (the filter and
combined waveforms are deliberately left out of the gate).
