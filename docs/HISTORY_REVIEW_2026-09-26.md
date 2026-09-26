# History and real-ROM follow-up — 2026-09-26

Reviewed all 32 commits from `3a8398b` through `f16918f`, including changes to
DSP, published IDs, state migration, native UI, tests, tools, and release workflows.
This supplements [the implementation review](REVIEW_2026-09-26.md).
The follow-up located historical Codex/Claude transcripts and the original ROM
directory; the earlier review's statement that transcripts were not found describes
that earlier search, not the evidence available for this follow-up.

## Findings and corrections

- **ROM-directory workflow retained.** `nes_preset_gate` still scans `NES_ROM_DIR`,
  selects the filename-derived score, applies its existing `tonal 0.95` gate,
  and exports the REAPER project and both WAVs. A missing directory now returns
  CTest's configured skip code 77. An existing but unusable directory still fails.
  This workflow does not emulate the selected game.
- **Oscillator checks are additive.** `f16918f` restored the directory workflow but
  removed the synthetic wrong-source control. A separate `nes_oscillator_gate`
  now compares the generated polyphonic score at `rom-mix 0.95` and requires
  wrong source, pulse duty, and octave controls to fail. It runs without ROMs.
  Neither the directory gate nor any real-ROM threshold was relaxed.
- **Real-ROM failures traced to capture setup.** SameBoy randomized initial RAM
  independently for its mix and four isolated-channel runs. Balloon Kid therefore
  compared different register streams. Each pass now starts with the same random
  seed, and the test rejects any solo register stream that differs from the mix.
  Turbo mode removes oracle wall-clock throttling. Super Mario Land and GB
  Battletoads require later Start presses; NES Zelda needs name-entry A presses.
  These recipes are captured in `tools/test_real_roms.sh`; no oscillator correction
  was needed to pass these cases. Missing requested games remain failures.
- **CLAP modulation implemented.** The README's host-modulation promise lacked
  `CLAP_EVENT_PARAM_MOD` handling. All 39 continuous parameters now advertise and
  consume global modulation offsets, clamped with the base value at the parameter
  bounds. Offsets replace previous offsets, arrive at their event sample, and do
  not change UI/base values or saved state. Reset clears them. Stepped and targeted
  per-note parameter modulation are not advertised. Existing note-expression
  support remains. Tests compare modulation with equivalent base automation,
  including block partitions, FM updates, clamping, state, and malformed events.
- **Published IDs preserved.** Comparing every historical parameter enum and
  source/preset table found append-only growth: 79 to 136 parameters, 51 to 59
  sources, and 40 to 81 factory presets (plus Manual). An independent checked-in
  snapshot now guards all 277 published IDs/names through public CLAP interfaces;
  future appended entries remain permitted. State v8–v15 migration into v16 remains.
- **Configured-build testing corrected.** Optional real-ROM CTests now use replay
  and comparison targets from the configured build directory, avoiding stale
  default-build binaries. External oracle cores remain separately supplied.

## Commit-by-commit disposition

“Retained” means the relevant implementation is still present and was included in
the current applicable test run; it is not a claim that every hardware behavior or
native platform was exhaustively certified.

| Commit | Contribution and disposition |
| --- | --- |
| `3a8398b` | Original instrument, source catalogue, parameter/state interfaces: retained; published numeric identities preserved. |
| `607cb92` | CI, license, README: retained; current accuracy claims considered against test scope below. |
| `db05f93` | Accuracy fixtures, comparison tools, DSP tests: retained in the expanded suites. |
| `271ebe4` | Presets and REAPER validation: retained and passing. |
| `f6d663a` | Volume/preset corrections: retained; current loudness, headroom, and recall tests pass. |
| `2c31023` | Native cross-platform build work: retained; non-Linux runtime validation remains outstanding. |
| `2341fc0` | Windows test/audio-comparison portability: retained. |
| `98c8751` | Windows subprocess command handling: retained. |
| `a6c38ad` | Source-specific chip defaults: retained. |
| `0ea4efe` | Manual/preset reset and parity comparison corrections: retained. |
| `7988d0c` | CLAP initialization and safe host callbacks: retained. |
| `8c0dd4c` | FM carrier mapping/levels, main-thread editor lifecycle and note fixes: retained. |
| `76222d0` | Parity, MIDI CC7, latency and UI hit testing: retained. |
| `f4ea938` | Game Boy recipes: retained with original preset IDs. |
| `276edc9` | Real-ROM parity tooling: retained and rerun; deterministic GB capture strengthened. |
| `744b6eb` | NES stack and cycle-stepped APU work: retained; directory and real-ROM lanes both pass. |
| `c18e286` | PCE accuracy and broader real-ROM lanes: retained; all three console lanes rerun. |
| `a1f7e46` | Preset voice-gain normalization: retained. |
| `8f7c076` | Expanded features/UI, duty macro and bend range: retained. |
| `19a95f9` | Rebuilt tracked binary only: superseded by source builds; no source feature removal. |
| `23e4035` | Windows/macOS build fixes: retained. |
| `dd659dd` | Windows cursor resource compatibility: retained. |
| `a79b598` | Portable pi constant in replay tools: retained. |
| `b39c040` | Windows `near` macro collision fix in tests: retained. |
| `6c80cb4` | Cents sequencer, vibrato delay, editor SIZE and release packages: retained. |
| `91ef8fa` | Per-channel bend/mod wheel, tail and transactional state fixes: retained. |
| `4b898db` | Editor screenshot documentation: retained through subsequent image replacements. |
| `e921d59` | Placeholder screenshot replaced by actual editor image: useful documentation retained. |
| `f800afd` | Voice-control screenshot: retained. |
| `e5b964b` | Build/install instructions: retained. |
| `90f378d` | Custom waveform, appended presets, FM/UI/state audit corrections: retained; migration and coverage tests pass. |
| `f16918f` | Restored ROM-directory workflow: retained, made optional when absent, with synthetic controls restored separately. |

Historical deleted files were the checked-in build binary and replaced placeholder
artwork. Renamed state round-trip tests did not remove old-state migration coverage.
No additional source/preset/parameter removal was found.

## Validation and limits

- Release: **94/94 CTests passed**, including all **63 Furnace comparisons**,
  public CLAP behavior, published IDs, presets/state/banks, frontend, REAPER,
  directory-backed NES, and additive oscillator checks.
- Debug ASan/UBSan/float-cast-overflow: **29/29 tests passed**; REAPER excluded
  and Furnace not enabled in that build. Leak detection disabled for this run.
- Real games: **26/26 passed** (14 NES, 8 GB/GBC, 4 PCE), using the existing
  **0.70** real-ROM threshold. Reproduce with
  `tools/test_real_roms.sh /mnt/crucial/roms "$PWD/build-reinstall"` after building
  the capture tools and patched external cores.
- Missing-directory check: `NES_ROM_DIR=/tmp/yanes-no-roms ctest --test-dir
  build-reinstall --output-on-failure -R 'nes_preset_gate|nes_oscillator_gate'`
  skipped only the directory-backed test; the additive oscillator test passed.
- A deliberately divergent GB solo register log was rejected before replay.
- CPU: the 118-case informational benchmark (59 sources, one/sixteen voices)
  produced finite output throughout. Constant-parameter modulation reads avoid
  runtime specification lookup. In the final comparison, median per-case mean
  block time was about 4% above the unmodulated build, and summed means about 7%
  above it; the highest mean real-time load was 28.2% versus 27.8%. Earlier runs
  varied substantially. Modulation is not claimed to be free or these measurements
  to guarantee DAW deadlines.
- Native X11 review passed with display access: three open/close cycles, all seven
  pages and preset picker at 50/75/100/125% scale, including actual waveform-edit
  clicks and screenshots. The initial sandbox-only attempt could not open X11.
  No fresh native Windows/macOS or Bitwig UI certification is claimed; CLAP
  modulation is verified through the public API.

The real-ROM lanes exercise independent emulator capture versus YANES register
replay, rather than proving the complete plugin reproduces every game. Furnace
covers 21 selected voices at three pitches, not all 59 sources or every register
combination. The generated-score gate uses two local APU implementations and is
not an independent external ROM oracle. Compact chip models and existing documented
hardware limitations remain. Passing this review establishes the listed retained
features and tested behavior, not universal cycle accuracy or absence of all bugs.
