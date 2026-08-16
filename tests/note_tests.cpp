// Note lifecycle coverage: CLAP note on/off/choke, note expressions, the raw MIDI
// dialect (including sustain pedal, All Sound Off and All Notes Off), transport tempo,
// voice stealing, and the stack mixer masks.
//
// Every assertion is made on rendered audio. With the console/TV section and the effects
// rack at their defaults the output is bit-exactly zero once no voice is active, so
// "the voice was released" is directly observable as silence.
#include "clap_harness.hpp"

using namespace harness;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 512;
// 512 frames at 48 kHz is ~10.7 ms, so 40 blocks comfortably outlives the 30 ms
// default release plus the one-block scheduling granularity.
constexpr int kReleaseBlocks = 40;

// A note that is sounding must be clearly audible, not merely non-zero.
constexpr float kAudible = 1.0e-3f;

void expect_silent(const Block& block, const char* what) {
  if (block.peak != 0.0f) {
    std::fprintf(stderr, "expected silence after %s, peak %.9f\n", what, static_cast<double>(block.peak));
    assert(false);
  }
}

void expect_audible(const Block& block, const char* what) {
  assert(block.finite);
  if (!(block.peak > kAudible)) {
    std::fprintf(stderr, "expected audio for %s, peak %.9f\n", what, static_cast<double>(block.peak));
    assert(false);
  }
}

// --- CLAP note dialect -------------------------------------------------------------

void test_note_on_off(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    expect_silent(runner.run(), "activation");

    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    expect_audible(runner.run(&on), "note on");
    expect_audible(runner.settle(4), "sustained note");

    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
    runner.run(&off);
    // The release ramp is audible for a while, then the voice must free itself.
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "note off release");
  }
  plugin->destroy(plugin);
}

// A long release must actually keep sounding, otherwise "silence after note off"
// would pass for the wrong reason.
void test_release_is_gradual(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 1000.0);

    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    runner.run(&on);
    runner.settle(2);

    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
    runner.run(&off);
    expect_audible(runner.settle(4), "1 s release still ringing");

    // 1000 ms release needs ~94 blocks; give it margin, then require silence.
    runner.settle(140);
    expect_silent(runner.run(), "1 s release completion");
  }
  plugin->destroy(plugin);
}

void test_note_choke_is_immediate(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 2000.0);

    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 7, 1.0));
    expect_audible(runner.run(&on), "note on before choke");

    Events choke;
    choke.push(note_event(CLAP_EVENT_NOTE_CHOKE, 0, 60, 7, 0.0));
    runner.run(&choke);
    // Choke clears the voice outright; it must not use the 2 s release ramp.
    expect_silent(runner.run(), "note choke");
  }
  plugin->destroy(plugin);
}

// CLAP lets a host address notes with -1 wildcards for channel, key and note id.
void test_note_off_wildcards(const Library& library) {
  struct Case { int16_t channel; int16_t key; int32_t note_id; const char* what; };
  const Case cases[] = {
      {0, 60, 5, "exact match"},
      {-1, 60, 5, "channel wildcard"},
      {0, -1, 5, "key wildcard"},
      {0, 60, -1, "note id wildcard"},
      {-1, -1, -1, "full wildcard"},
  };
  for (const Case& test : cases) {
    const clap_plugin_t* plugin = library.create();
    {
      Runner runner(plugin, kRate, kBlock);
      Events on;
      on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 5, 1.0));
      expect_audible(runner.run(&on), test.what);

      Events off;
      off.push(note_event(CLAP_EVENT_NOTE_OFF, test.channel, test.key, test.note_id, 0.0));
      runner.run(&off);
      runner.settle(kReleaseBlocks);
      expect_silent(runner.run(), test.what);
    }
    plugin->destroy(plugin);
  }
}

// A note off that matches nothing must leave the sounding voice alone.
void test_non_matching_note_off(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 5, 1.0));
    runner.run(&on);

    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 64, 6, 0.0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_audible(runner.run(), "note held through a non-matching note off");
  }
  plugin->destroy(plugin);
}

// --- note expressions --------------------------------------------------------------

void test_note_expressions(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 3, 1.0));
    const Block plain = runner.run(&on);
    expect_audible(plain, "note before expressions");

    Events volume;
    volume.push(expression_event(CLAP_NOTE_EXPRESSION_VOLUME, 0, 60, 3, 0.0));
    runner.run(&volume);
    expect_silent(runner.run(), "zero volume expression");

    Events restore;
    restore.push(expression_event(CLAP_NOTE_EXPRESSION_VOLUME, 0, 60, 3, 1.0));
    runner.run(&restore);
    expect_audible(runner.run(), "restored volume expression");

    // Tuning, brightness and pressure all change the rendered signal.
    for (const auto id : {CLAP_NOTE_EXPRESSION_TUNING, CLAP_NOTE_EXPRESSION_BRIGHTNESS,
                          CLAP_NOTE_EXPRESSION_PRESSURE}) {
      const Block before = runner.run();
      Events change;
      change.push(expression_event(id, 0, 60, 3, id == CLAP_NOTE_EXPRESSION_TUNING ? 7.0 : 1.0));
      runner.run(&change);
      const Block after = runner.run();
      assert(after.finite);
      assert(std::abs(after.rms - before.rms) > 1.0e-6 || id == CLAP_NOTE_EXPRESSION_TUNING);
    }
  }
  plugin->destroy(plugin);
}

// --- raw MIDI dialect --------------------------------------------------------------

void test_midi_note_on_off(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    on.push(midi_event(0x90, 60, 100));
    expect_audible(runner.run(&on), "MIDI note on");

    Events off;
    off.push(midi_event(0x80, 60, 0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "MIDI note off");

    // Running-status style note off: note on with velocity zero.
    Events on_again;
    on_again.push(midi_event(0x90, 62, 100));
    expect_audible(runner.run(&on_again), "second MIDI note on");
    Events zero_velocity;
    zero_velocity.push(midi_event(0x90, 62, 0));
    runner.run(&zero_velocity);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "MIDI note on with velocity zero");
  }
  plugin->destroy(plugin);
}

void test_sustain_pedal(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 127));   // pedal down
    sequence.push(midi_event(0x90, 60, 100));   // note on
    expect_audible(runner.run(&sequence), "note under a held pedal");

    Events off;
    off.push(midi_event(0x80, 60, 0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_audible(runner.run(), "note sustained by the pedal after note off");

    Events release_pedal;
    release_pedal.push(midi_event(0xb0, 64, 0));
    runner.run(&release_pedal);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "pedal release");
  }
  plugin->destroy(plugin);
}

// The CLAP note dialect must honour the pedal too, not just raw MIDI.
void test_sustain_pedal_with_clap_notes(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 127));
    sequence.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 11, 1.0));
    runner.run(&sequence);

    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 11, 0.0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_audible(runner.run(), "CLAP note sustained by the pedal");

    Events release_pedal;
    release_pedal.push(midi_event(0xb0, 64, 0));
    runner.run(&release_pedal);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "pedal release for a CLAP note");
  }
  plugin->destroy(plugin);
}

// A pedal press below 64 counts as up, per the MIDI specification.
void test_pedal_threshold(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 63));
    sequence.push(midi_event(0x90, 60, 100));
    runner.run(&sequence);

    Events off;
    off.push(midi_event(0x80, 60, 0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "note off with the pedal below the threshold");
  }
  plugin->destroy(plugin);
}

// All Sound Off (120) silences immediately, even under the sustain pedal.
// All Notes Off (123) is a channel-wide note off: it honors sustain and release.
void test_panic_controllers(const Library& library) {
  {
    const clap_plugin_t* plugin = library.create();
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 2000.0);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 127));
    for (uint8_t key = 60; key < 66; ++key) sequence.push(midi_event(0x90, key, 100));
    expect_audible(runner.run(&sequence), "chord before All Sound Off");
    Events panic;
    panic.push(midi_event(0xb0, 120, 0));
    runner.run(&panic);
    expect_silent(runner.run(), "All Sound Off");
    plugin->destroy(plugin);
  }
  {
    const clap_plugin_t* plugin = library.create();
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 2000.0);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 127));
    sequence.push(midi_event(0x90, 60, 100));
    expect_audible(runner.run(&sequence), "held before All Notes Off");
    Events panic;
    panic.push(midi_event(0xb0, 123, 0));
    runner.run(&panic);
    expect_audible(runner.run(), "All Notes Off still sustained by pedal");
    Events pedal_up;
    pedal_up.push(midi_event(0xb0, 64, 0));
    runner.run(&pedal_up);
    expect_audible(runner.settle(4), "All Notes Off release after pedal up");
    runner.settle(220);
    expect_silent(runner.run(), "All Notes Off completed");
    plugin->destroy(plugin);
  }
  {
    const clap_plugin_t* plugin = library.create();
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 30.0);
    Events on;
    on.push(midi_event(0x90, 60, 100));
    expect_audible(runner.run(&on), "note before All Notes Off");
    Events panic;
    panic.push(midi_event(0xb0, 123, 0));
    runner.run(&panic);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "All Notes Off without pedal");
    plugin->destroy(plugin);
  }
}

// Panic is per channel, matching the MIDI specification.
void test_panic_is_per_channel(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events sequence;
    sequence.push(midi_event(0x90 | 0, 60, 100));
    sequence.push(midi_event(0x90 | 1, 67, 100));
    runner.run(&sequence);

    Events panic;
    panic.push(midi_event(0xb0 | 0, 123, 0));
    runner.run(&panic);
    expect_audible(runner.run(), "voice on channel 2 survives a channel 1 panic");
  }
  plugin->destroy(plugin);
}

void test_mod_wheel_and_pitch_bend(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    on.push(midi_event(0x90, 60, 100));
    runner.run(&on);
    const Block plain = runner.settle(2);

    Events wheel;
    wheel.push(midi_event(0xb0, 1, 127));
    runner.run(&wheel);
    const Block vibrato = runner.settle(2);
    assert(vibrato.finite);
    // The mod wheel adds vibrato, which changes the rendered waveform.
    assert(std::abs(vibrato.rms - plain.rms) > 1.0e-9);

    // Pitch bend: centre is 8192, encoded low 7 bits then high 7 bits.
    Events bend;
    bend.push(midi_event(0xe0, 0, 127));  // maximum upward bend
    runner.run(&bend);
    const Block bent = runner.settle(2);
    assert(bent.finite && bent.peak > kAudible);

    Events centre;
    centre.push(midi_event(0xe0, 0, 64));  // 8192 = no bend
    runner.run(&centre);
    assert(runner.settle(2).finite);
  }
  plugin->destroy(plugin);
}

// Channel pressure and other unhandled status bytes must be ignored, not misparsed.
void test_unhandled_midi_is_ignored(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    on.push(midi_event(0x90, 60, 100));
    runner.run(&on);
    const double before = runner.settle(2).rms;

    Events noise;
    noise.push(midi_event(0xd0, 100, 0));   // channel pressure
    noise.push(midi_event(0xa0, 60, 100));  // polyphonic aftertouch
    noise.push(midi_event(0xc0, 42, 0));    // program change
    noise.push(midi_event(0xb0, 7, 0));     // channel volume, not implemented
    runner.run(&noise);
    const Block after = runner.settle(2);
    assert(after.finite && after.peak > kAudible);
    assert(std::abs(after.rms - before) < 0.5);  // still the same voice, not silenced
  }
  plugin->destroy(plugin);
}

// --- polyphony ---------------------------------------------------------------------

void test_polyphony_and_voice_stealing(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events chord;
    // 20 notes against 16 voices forces the oldest four to be stolen.
    for (int16_t i = 0; i < 20; ++i) chord.push(note_event(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(48 + i), i, 1.0));
    const Block block = runner.run(&chord);
    expect_audible(block, "20 overlapping notes");
    assert(block.finite);

    // Releasing every note by wildcard must still reach silence: no voice may be
    // left stuck by the stealing path.
    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, -1, -1, -1, 0.0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "wildcard note off after voice stealing");
  }
  plugin->destroy(plugin);
}

// reset() must drop every voice, including sustained ones, without a release tail.
void test_reset_clears_voices(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Release"), 2000.0);
    Events sequence;
    sequence.push(midi_event(0xb0, 64, 127));
    sequence.push(midi_event(0x90, 60, 100));
    sequence.push(midi_event(0xb0, 1, 127));
    sequence.push(midi_event(0xe0, 0, 127));
    expect_audible(runner.run(&sequence), "voice before reset");

    plugin->reset(plugin);
    expect_silent(runner.run(), "reset");
  }
  plugin->destroy(plugin);
}

// --- transport ---------------------------------------------------------------------

void test_transport_tempo(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Arpeggio"), 5);      // user step sequence
    runner.set(find_param(plugin, "Tempo sync"), 1);
    runner.set(find_param(plugin, "Sync division"), 7);  // fastest division

    clap_event_transport_t transport{};
    transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO;

    Events slow;
    slow.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    transport.tempo = 60.0;
    slow.push(transport);
    runner.run(&slow);
    const double slow_rms = runner.settle(8).rms;

    plugin->reset(plugin);
    Events fast;
    fast.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    transport.tempo = 220.0;
    fast.push(transport);
    runner.run(&fast);
    const Block fast_block = runner.settle(8);
    assert(fast_block.finite && fast_block.peak > kAudible);
    // Different tempos step the sequence at different rates.
    assert(std::abs(fast_block.rms - slow_rms) > 1.0e-9);

    // A transport event without the tempo flag must be ignored rather than
    // installing a zero tempo.
    clap_event_transport_t flagless = transport;
    flagless.flags = 0;
    flagless.tempo = 0.0;
    Events ignored;
    ignored.push(flagless);
    runner.run(&ignored);
    assert(runner.settle(2).finite);
  }
  plugin->destroy(plugin);
}

// --- stack mixer masks --------------------------------------------------------------

void test_mixer_masks(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    const clap_id mute = find_param(plugin, "Channel mute mask");
    const clap_id solo = find_param(plugin, "Channel solo mask");
    runner.set(find_param(plugin, "Waveform"), 18);  // NES five-channel stack

    Events chord;
    chord.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    chord.push(note_event(CLAP_EVENT_NOTE_ON, 1, 64, 2, 1.0));
    runner.run(&chord);
    expect_audible(runner.settle(2), "two stack channels");

    runner.set(mute, 0x0003);  // mute both sounding channels
    expect_silent(runner.settle(2), "both channels muted");

    runner.set(mute, 0);
    expect_audible(runner.settle(2), "unmuted");

    // Soloing a silent channel mutes everything else.
    runner.set(solo, 0x0004);
    expect_silent(runner.settle(2), "solo on an unused channel");

    runner.set(solo, 0x0001);
    expect_audible(runner.settle(2), "solo on a sounding channel");

    runner.set(solo, 0);
    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, -1, -1, -1, 0.0));
    runner.run(&off);
    runner.settle(kReleaseBlocks);
    expect_silent(runner.run(), "stack note off");
  }
  plugin->destroy(plugin);
}

// Strict Hardware chokes an existing voice when its stack channel is retriggered.
void test_strict_hardware_choke(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    runner.set(find_param(plugin, "Waveform"), 18);
    runner.set(find_param(plugin, "Strict hardware"), 1);
    runner.set(find_param(plugin, "Release"), 2000.0);

    Events first;
    first.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
    runner.run(&first);

    Events second;
    second.push(note_event(CLAP_EVENT_NOTE_ON, 0, 72, 2, 1.0));
    runner.run(&second);

    // Only the retriggered voice remains; releasing it must reach silence even though
    // the first note was never given a note off.
    Events off;
    off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 72, 2, 0.0));
    runner.run(&off);
    runner.settle(220);
    expect_silent(runner.run(), "strict hardware retrigger");
  }
  plugin->destroy(plugin);
}

// --- sample rates and block sizes ---------------------------------------------------

void test_rates_and_block_sizes(const Library& library) {
  for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
    for (const uint32_t frames : {uint32_t{1}, uint32_t{16}, uint32_t{64}, uint32_t{1024}}) {
      const clap_plugin_t* plugin = library.create();
      {
        Runner runner(plugin, rate, frames);
        Events on;
        on.push(note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0));
        Block block = runner.run(&on);
        assert(block.finite);
        for (int i = 0; i < 64; ++i) { block = runner.run(); assert(block.finite); }

        Events off;
        off.push(note_event(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0));
        runner.run(&off);
        // Silence must be reached regardless of rate or block size.
        runner.settle(static_cast<int>(rate * 0.2 / frames) + 4);
        expect_silent(runner.run(), "note off at an alternate rate or block size");
      }
      plugin->destroy(plugin);
    }
  }
}

// Events landing mid-block must be applied at their frame, not dropped.
void test_sample_accurate_event_timing(const Library& library) {
  const clap_plugin_t* plugin = library.create();
  {
    Runner runner(plugin, kRate, kBlock);
    Events on;
    clap_event_note_t note = note_event(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
    note.header.time = kBlock / 2;
    on.push(note);
    runner.run(&on);

    // Nothing before the event frame, audio after it.
    float before = 0.0f, after = 0.0f;
    for (uint32_t i = 0; i < kBlock / 2; ++i) before = std::max(before, std::abs(runner.left()[i]));
    for (uint32_t i = kBlock / 2; i < kBlock; ++i) after = std::max(after, std::abs(runner.left()[i]));
    assert(before == 0.0f);
    assert(after > kAudible);
  }
  plugin->destroy(plugin);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const Library library(argv[1]);

  test_note_on_off(library);
  test_release_is_gradual(library);
  test_note_choke_is_immediate(library);
  test_note_off_wildcards(library);
  test_non_matching_note_off(library);
  test_note_expressions(library);
  test_midi_note_on_off(library);
  test_sustain_pedal(library);
  test_sustain_pedal_with_clap_notes(library);
  test_pedal_threshold(library);
  test_panic_controllers(library);
  test_panic_is_per_channel(library);
  test_mod_wheel_and_pitch_bend(library);
  test_unhandled_midi_is_ignored(library);
  test_polyphony_and_voice_stealing(library);
  test_reset_clears_voices(library);
  test_transport_tempo(library);
  test_mixer_masks(library);
  test_strict_hardware_choke(library);
  test_rates_and_block_sizes(library);
  test_sample_accurate_event_timing(library);
  std::printf("note_tests: all checks passed\n");
}
