// Which parameters the editor shows, where, and as what kind of control. Grouped by what a
// musician is adjusting rather than by parameter ID; the tests check that every parameter is
// reachable from exactly one place.
#pragma once

#include <vector>

#include "params.hpp"
#include "ui_editor.hpp"

namespace yanes::ui {

inline std::vector<PageSpec> yanes_pages() {
  using namespace yanes::params;
  using W = Widget;
  const auto knob = [](int id) { return ControlSpec{W::Knob, id}; };
  const auto toggle = [](int id) { return ControlSpec{W::Toggle, id}; };
  const auto segments = [](int id, int span) { return ControlSpec{W::Segments, id, span}; };
  const auto menu = [](int id, int span) { return ControlSpec{W::Menu, id, span}; };
  const auto lane = [](int first, int steps, int length, const char* caption) {
    return ControlSpec{W::Lane, first, 1, steps, length, caption};
  };
  return {
      {"VOICE", "Pick a sound source and shape how it plays  •  hover any control for help", theme::cyan, 3, 112,
       {{"Sound source", 0,
         {menu(kWaveform, 3), segments(kDuty, 3), knob(kExpansionShape), knob(kNoisePeriod), toggle(kNoiseMode),
          knob(kFmRatio), knob(kFmIndex)}},
        {"Envelope", 1, {knob(kAttackMs), knob(kReleaseMs), toggle(kHardwareEnvelope), knob(kEnvelopeRate)}},
        {"Level", 1, {knob(kGainDb), knob(kMasterDb), toggle(kVelocity), knob(kStereoWidth)}},
        {"Pitch", 2,
         {knob(kTranspose), knob(kFineTune), knob(kPitchBendRange), knob(kPortamentoMs), knob(kVibratoRate),
          knob(kVibratoDepth), knob(kVibratoDelay)}}}},
      {"SEQUENCE", "Tracker-style macros: paint pitch, duty, and cents steps that restart with every note", theme::amber, 2, 0,
       {{"Pitch steps", 0, {menu(kArpMode, 2), knob(kArpRate), knob(kSequenceLength), lane(kSequence1, 8, kSequenceLength, "Semitones per step")}},
        {"Duty steps", 0, {segments(kDutySeqMode, 2), knob(kDutySeqRate), knob(kDutySeqLength), lane(kDutyStep1, 8, kDutySeqLength, "Pulse duty per step")}},
        {"Timing", 1, {toggle(kTempoSync), knob(kSyncDivision)}},
        {"Cents steps", 1, {segments(kCentsSeqMode, 2), knob(kCentsSeqRate), knob(kCentsSeqLength), lane(kCentsStep1, 8, kCentsSeqLength, "Cents per step")}},
        {"Pitch sweep", 1, {knob(kSweepDepth), knob(kSweepTime)}}}},
      {"SYNTH", "Original digital tones: wavetables, additive partials, the chip filter, and layers", theme::green, 3, 112,
       {{"Wavetable", 0, {knob(kWavetablePosition), knob(kWavetableWarp), knob(kAdditiveTilt)}},
        {"Chip filter", 1, {knob(kChipCutoff), knob(kChipResonance)}},
        {"Layer", 2, {menu(kLayerMode, 2), knob(kLayerMix)}}}},
      {"FM", "Hardware FM: algorithm, operator envelopes, and the chip LFO", theme::pink, 3, 112,
       {{"Algorithm", 0, {knob(kGenesisAlgorithm), knob(kGenesisFeedback), knob(kFmBrightness)}},
        {"Operators", 1,
         {knob(kFmAttack), knob(kFmDecay), knob(kFmSustainRate), knob(kFmSustainLevel), knob(kFmRelease),
          knob(kFmDetune), knob(kFmKeyScale)}},
        {"LFO", 2, {knob(kFmLfoRate), knob(kFmAmDepth), knob(kFmPmDepth)}}}},
      {"HARDWARE", "Console timing, channel stacks, and the one-bit DPCM sample bank", theme::red, 3, 112,
       {{"Console", 0, {segments(kClockMode, 3), toggle(kStrictHardware)}},
        {"DPCM playback", 1, {knob(kDpcmRate), knob(kDpcmBaseKey), knob(kDpcmInitialLevel)}},
        {"DPCM trim", 2, {knob(kDpcmTrimStart), knob(kDpcmTrimEnd)}}}},
      {"FX + TV", "Effects rack and the console-into-television output chain", theme::violet, 3, 0,
       {{"Drive + echo", 0, {knob(kDrive), knob(kEchoMix), knob(kEchoTime), knob(kEchoFeedback)}},
        {"Chorus", 0, {knob(kChorusMix), knob(kChorusRate), knob(kChorusDepth)}},
        {"Console + TV", 1,
         {knob(kRetroAmount), knob(kBitDepth), knob(kOutputRate), knob(kRfNoise), knob(kHum), knob(kSpeaker)}}}},
      {"CUSTOM", "Draw one repeating cycle • enable Custom wave to replace the source oscillator", theme::cyan, 1, 0,
       {{"Custom waveform · 32 samples / 4-bit levels", 0,
         {toggle(kCustomWave), lane(kWaveSample1, 32, -1, "Wave levels · 0 to 15")}}}},
  };
}

// Names for the knob cells, where the section title already supplies the context ("FM",
// "DPCM", ...). Null means the full parameter name fits.
inline const char* yanes_short_name(int id) {
  using namespace yanes::params;
  switch (id) {
    case kHardwareEnvelope: return "HW envelope";
    case kPitchBendRange: return "Bend range";
    case kSequenceLength: return "Steps";
    case kDutySeqLength: return "Steps";
    case kDutySeqRate: return "Step rate";
    case kDutySeqMode: return "Duty sequence";
    case kCentsSeqLength: return "Steps";
    case kCentsSeqRate: return "Step rate";
    case kCentsSeqMode: return "Cents sequence";
    case kVibratoDelay: return "Vib delay";
    case kVibratoRate: return "Vib rate";
    case kVibratoDepth: return "Vib depth";
    case kArpRate: return "Step rate";
    case kFmAttack: return "Attack";
    case kFmDecay: return "Decay";
    case kFmSustainRate: return "Sustain rate";
    case kFmSustainLevel: return "Sustain level";
    case kFmRelease: return "Release";
    case kFmDetune: return "Detune";
    case kFmKeyScale: return "Key scale";
    case kFmLfoRate: return "LFO rate";
    case kFmAmDepth: return "AM depth";
    case kFmPmDepth: return "PM depth";
    case kGenesisAlgorithm: return "Algorithm";
    case kGenesisFeedback: return "Feedback";
    case kFmBrightness: return "Brightness";
    case kDpcmInitialLevel: return "Initial level";
    case kDpcmTrimStart: return "Trim start";
    case kDpcmTrimEnd: return "Trim end";
    case kDpcmBaseKey: return "Base key";
    case kStrictHardware: return "Strict HW";
    default: return nullptr;
  }
}

inline ControlSpec yanes_header_control() { return ControlSpec{Widget::Menu, yanes::params::kPreset}; }

// Parameters edited through page artwork (mixer and bank tiles) rather than a card control.
inline std::vector<int> yanes_strip_params() {
  using namespace yanes::params;
  return {kStackMuteMask, kStackSoloMask, kDpcmLoopMask};
}

}  // namespace yanes::ui
