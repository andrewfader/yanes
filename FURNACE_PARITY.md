# Furnace audio parity

## Composite parity gate

The authoritative gate now requires every fixture to satisfy all applicable checks:

- envelope correlation at least 0.80;
- spectral similarity at least 0.80 (harmonic-energy cosine similarity for tonal
  fixtures; log-band correlation for stochastic noise);
- onset and offset within 30 ms;
- tonal pitch within 20 cents (noise fixtures omit pitch).

Current result using Furnace `dev250` commit `389a6cced442c62afcafef2865a35a8e83db87a2`:
**21/21 pass**. Each row below independently clears every
applicable threshold; this is not an aggregate coverage percentage.

| Fixture | Envelope | Spectrum | Onset ms | Offset ms | Pitch cents | Result |
|---|---:|---:|---:|---:|---:|:---:|
| ay-tone | 0.990235 | 0.999989 | 0.000 | 5.805 | 3.310 | Pass |
| fds | 0.990824 | 0.943462 | 0.000 | 0.000 | 0.000 | Pass |
| gameboy-noise | 0.858881 | 0.879935 | 0.000 | 11.610 | n/a | Pass |
| gameboy-pulse | 0.848377 | 0.999963 | 0.000 | 5.805 | 1.654 | Pass |
| gameboy-wave | 0.977907 | 0.884439 | 0.000 | 0.000 | 0.000 | Pass |
| n163 | 0.975681 | 0.855572 | 0.000 | 23.220 | 1.654 | Pass |
| nes-noise | 0.879281 | 0.996420 | 0.000 | 17.415 | n/a | Pass |
| nes-pulse | 0.985336 | 0.999898 | 0.000 | 11.610 | 0.000 | Pass |
| nes-triangle | 0.983159 | 0.994772 | 0.000 | 17.415 | 0.000 | Pass |
| pce-wave | 0.991773 | 0.996817 | 0.000 | 0.000 | 1.655 | Pass |
| pokey-tone | 0.987764 | 0.999949 | 0.000 | 17.415 | 0.000 | Pass |
| saa1099 | 0.989804 | 0.999996 | 0.000 | 5.805 | 1.653 | Pass |
| scc | 0.981813 | 0.897768 | 0.000 | 29.025 | 1.655 | Pass |
| sid6581 | 0.843980 | 0.961649 | 0.000 | 5.805 | 1.654 | Pass |
| sid8580 | 0.816455 | 0.965772 | 0.000 | 23.220 | 0.000 | Pass |
| sms-noise | 0.972106 | 0.936901 | 11.610 | 17.415 | n/a | Pass |
| sms-tone | 0.990007 | 0.999900 | 0.000 | 5.805 | 0.000 | Pass |
| tia | 0.989621 | 0.999993 | 0.000 | 5.805 | 0.000 | Pass |
| vrc6-pulse | 0.941688 | 0.999814 | 11.610 | 17.415 | 1.655 | Pass |
| vrc6-saw | 0.983688 | 0.981681 | 11.610 | 17.415 | 2.480 | Pass |
| vrc7 | 0.913595 | 0.819214 | 0.000 | 0.000 | 6.627 | Pass |

## Untuned octave holdouts

Every fixture is also generated one octave below and above the original note. The YANES renderer
uses the same waveform, duty, shape, noise, and envelope settings; only the MIDI key changes.
These 42 cases are intentionally not tuned individually.

Current result: **37/42 holdouts pass**, for **58/63 audio-parity cases overall**. The five
failures remain enabled as failing CTests when the Furnace suite is configured:

| Holdout | Envelope | Spectrum | Timing issue | Pitch cents | Failing requirement |
|---|---:|---:|---:|---:|---|
| gameboy-noise-low | 0.757735 | 0.983310 | none | n/a | envelope |
| gameboy-noise-high | 0.853476 | 0.685832 | none | n/a | spectrum |
| nes-noise-high | 0.987451 | 0.721037 | none | n/a | spectrum |
| tia-low | 0.988514 | 0.914224 | none | 57.208 | pitch |
| vrc7-low | 0.923628 | 0.816825 | 58.050 ms offset | 3.313 | offset timing |

## Envelope-only historical baseline

Fixtures were generated and rendered with the pinned Furnace commit above; YANES was rendered at 48 kHz with
its clean output path. Audio is mixed to mono and resampled before comparison. The acceptance
target is **0.80 envelope correlation**. Waveform correlation is informational because independent
chip cores do not share oscillator phase.

| Fixture | YANES mode | Envelope | Waveform | RMS error | 0.80 target |
|---|---|---:|---:|---:|:---:|
| ay-tone | AY-3-8910 tone | 0.997235 | 0.006253 | 0.147303 | Pass |
| fds | FDS wavetable | 0.987934 | 0.008146 | 0.111991 | Pass |
| gameboy-noise | Game Boy noise | 0.861728 | 0.000664 | 0.070858 | Pass |
| gameboy-pulse | Game Boy pulse | 0.856916 | -0.146494 | 0.069761 | Pass |
| gameboy-wave | Game Boy wave | 0.991294 | -0.377792 | 0.119714 | Pass |
| n163 | Namco 163 wavetable | 0.999878 | -0.590147 | 0.105078 | Pass |
| nes-noise | NES noise | 0.981371 | -0.007359 | 0.159886 | Pass |
| nes-pulse | NES pulse | 0.999455 | 0.197550 | 0.123659 | Pass |
| nes-triangle | NES triangle | 0.998505 | 0.727109 | 0.074478 | Pass |
| pce-wave | PC Engine wavetable | 0.991256 | 0.034554 | 0.077302 | Pass |
| pokey-tone | POKEY tone | 0.999996 | -0.005752 | 0.157884 | Pass |
| saa1099 | SAA1099 tone | 0.997290 | 0.074713 | 0.128420 | Pass |
| scc | Konami SCC | 0.999956 | 0.071920 | 0.083897 | Pass |
| sid6581 | SID 6581 | 0.801834 | 0.002154 | 0.027434 | Pass |
| sid8580 | SID 8580 | 0.806207 | 0.158700 | 0.026536 | Pass |
| sms-noise | SMS noise | 0.976243 | -0.010654 | 0.174461 | Pass |
| sms-tone | SMS tone | 0.996993 | -0.000682 | 0.175185 | Pass |
| tia | Atari TIA | 0.997295 | -0.002224 | 0.167577 | Pass |
| vrc6-pulse | VRC6 pulse | 0.923413 | -0.038484 | 0.126587 | Pass |
| vrc6-saw | VRC6 saw | 0.962682 | 0.010898 | 0.133370 | Pass |
| vrc7 | VRC7 FM | 0.954141 | -0.025591 | 0.109251 | Pass |

Summary: **21/21 pass** the older envelope-only 0.80 target. This table is retained for diagnosis,
but it is no longer sufficient for the parity claim.
