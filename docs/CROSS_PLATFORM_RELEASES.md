# Cross-platform build and release process

YANES builds its audio engine, CLAP interface, parameters, automation, state, and command-line
tools on Linux, Windows, and macOS. The custom editor currently remains a Linux/X11 feature;
Windows and macOS builds use the host's generic parameter interface until native Win32 and Cocoa
editor backends are implemented.

## Continuous integration

`.github/workflows/ci.yml` builds and tests four pinned targets:

- Ubuntu 24.04 x64
- Windows Server 2022 x64
- macOS 15 Apple Silicon
- macOS 15 Intel

Every job configures a Release build, runs the complete applicable CTest suite, installs into a
staging directory, and uploads an unsigned artifact. Pull-request jobs never receive signing
credentials.

## Release promotion

Promote a tested commit rather than rebuilding unrelated source:

1. Tag the exact tested commit.
2. Re-run the matrix from the protected tag.
3. Build a macOS universal binary from the same source using
   `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` and repeat CTest on native ARM and Intel runners.
4. Sign the Windows binary and installer with an Authenticode certificate and timestamp them.
5. Sign the macOS bundle with a Developer ID Application certificate and hardened runtime.
6. Submit the signed macOS archive with `notarytool`, inspect the notarization log, and staple the
   ticket to the distributable container.
7. Install the packaged artifacts on clean Windows 11 and macOS machines, scan them in at least
   one CLAP host, recall state, exercise automation, and render known MIDI.
8. Publish the already-tested packages from a protected GitHub Environment.

Signing and notarization belong in a separate tag-only workflow guarded by required reviewers.
Store certificates and API credentials as environment secrets, never repository secrets exposed
to general CI.

## Native host certification

GitHub-hosted machines are appropriate for compilation and the headless CLAP harness. Persistent
self-hosted runners should be reserved for licensed DAWs and GUI/installer acceptance tests. Do
not route pull requests from forks to those machines. Recommended labels are
`[self-hosted, windows, x64, audio-host]` and `[self-hosted, macOS, ARM64, audio-host]`.

The next product milestone is to extract the existing drawing operations behind a small editor
backend, retain X11/Xft on Linux, and implement Win32 plus Cocoa versions. Platform-neutral layout
tests should remain shared; native smoke tests should verify window embedding, resize behavior,
file selection, DPI/Retina scaling, and editor teardown in a real host.
