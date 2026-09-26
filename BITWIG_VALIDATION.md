# Bitwig acceptance checklist

YANES is a native CLAP instrument with a custom editor on Linux (X11, including XWayland),
Windows, and macOS. The audio engine itself has no display-server dependency.

1. Restart Bitwig or open **Settings > Locations > Plug-in Locations** and rescan plug-ins.
2. Add **YANES** from the CLAP instrument list and open its editor.
3. Confirm all seven pages render: Voice, Sequence, Synth, FM, Hardware, FX + TV, and Custom.
4. Resize or scale the Bitwig window and verify mouse focus, parameter dragging, and keyboard input.
5. Automate a parameter, add Bitwig modulation, and confirm undo records one gesture per edit.
6. In an FM mode, hold a note while changing algorithm, feedback, brightness, envelope, detune, or
   LFO controls; the sounding note should update without retriggering. Test pitch bend and glide too.
7. In NES DPCM mode, middle-click a bank slot to load a 16-bit PCM WAV or `.ydmc`, left-click to
   toggle looping, and right-click to clear it. Exercise Base Key, Initial Level, and Trim Start/End.
8. On the Hardware page, left-click channel strips to mute and right-click to solo.
9. Save the project, close it, reopen it, and confirm preset, automation, mixer masks, sequences, and
   the custom waveform and all DPCM bank data return without the original sample files being present.
10. Test several simultaneous instances at 44.1, 48, and 96 kHz and at the smallest practical audio
    buffer. Check for stuck notes using sustain, transport stop, All Notes Off, and All Sound Off.

For a Wayland diagnostic, `echo $XDG_SESSION_TYPE` should report `wayland` and `echo $DISPLAY`
should still contain an XWayland display. A missing `$DISPLAY` prevents the optional editor from
opening but does not prevent Bitwig's native parameter panel or the synth engine from working.

11. Select Custom wavetable, draw across the whole lane in a quick stroke, automate samples,
    and verify recall. Try the Game Boy custom bass preset and switch the override off/on.
12. Verify NES-stack pitch bend, channel CC7 volume, and Layer Mix while holding a note.
13. Verify FM note-off does not restart the envelope and A4 is 440 Hz for each hardware FM family.
14. On Linux, open a sample dialog and close the editor before dismissing it; the host must remain responsive.

This is a manual acceptance checklist, not a claim that these native GUI checks ran in CI.
