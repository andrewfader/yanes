// The parameter table, value names, and value formatting. Shared by the plug-in, the editor, and
// the tests, so the editor's page layout can be checked against the real parameter list.
//
// Parameter IDs are the host's automation handles and index the saved state, so a new parameter
// is only ever appended just before kParamCount.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace yanes::params {

enum ParamId : uint32_t {
  kWaveform, kDuty, kNoisePeriod, kNoiseMode, kAttackMs, kReleaseMs,
  kExpansionShape, kFmRatio, kFmIndex, kGainDb, kVelocity, kTranspose, kFineTune,
  kPortamentoMs, kMasterDb, kClockMode, kHardwareEnvelope, kEnvelopeRate,
  kSweepDepth, kSweepTime, kArpMode, kArpRate, kDpcmRate, kGenesisAlgorithm,
  kGenesisFeedback, kRetroAmount, kBitDepth, kOutputRate, kRfNoise, kHum,
  kSpeaker, kStereoWidth, kChipCutoff, kChipResonance, kWavetablePosition,
  kWavetableWarp, kAdditiveTilt, kFmBrightness, kLayerMode, kLayerMix,
  kVibratoRate, kVibratoDepth, kDrive, kEchoMix, kEchoTime, kEchoFeedback,
  kChorusMix, kChorusRate, kChorusDepth, kTempoSync, kSyncDivision,
  kStrictHardware, kSequenceLength, kSequence1, kSequence2, kSequence3,
  kSequence4, kSequence5, kSequence6, kSequence7, kSequence8,
  kFmAttack, kFmDecay, kFmSustainRate, kFmSustainLevel, kFmRelease,
  kFmDetune, kFmKeyScale, kFmLfoRate, kFmAmDepth, kFmPmDepth, kDpcmBaseKey,
  kDpcmLoopMask, kDpcmInitialLevel, kDpcmTrimStart, kDpcmTrimEnd, kStackMuteMask, kStackSoloMask,
  kPreset, kPitchBendRange, kDutySeqMode, kDutySeqLength, kDutySeqRate,
  kDutyStep1, kDutyStep2, kDutyStep3, kDutyStep4, kDutyStep5, kDutyStep6,
  kDutyStep7, kDutyStep8, kVibratoDelay, kCentsSeqMode, kCentsSeqLength, kCentsSeqRate,
  kCentsStep1, kCentsStep2, kCentsStep3, kCentsStep4, kCentsStep5, kCentsStep6,
  kCentsStep7, kCentsStep8, kParamCount
};

struct ParamSpec {
  const char* name;
  const char* module;
  double min;
  double max;
  double def;
  bool stepped;
};

constexpr std::array<ParamSpec, kParamCount> kSpecs{{
    {"Waveform", "Oscillator", 0, 57, 0, true},
    {"Pulse duty", "Oscillator", 0, 3, 1, true},
    {"Noise period", "Oscillator/Noise", 0, 15, 8, true},
    {"Noise mode", "Oscillator/Noise", 0, 1, 0, true},
    {"Attack", "Envelope", 0, 500, 2, false},
    {"Release", "Envelope", 0, 2000, 30, false},
    {"Shape", "Expansion audio", 0, 7, 3, true},
    {"FM ratio", "Expansion audio/VRC7", 0.5, 8, 2, false},
    {"FM index", "Expansion audio/VRC7", 0, 8, 2, false},
    {"Voice gain", "Output", -36, 6, -9, false},
    {"Velocity", "Performance", 0, 1, 1, true},
    {"Transpose", "Performance", -24, 24, 0, true},
    {"Fine tune", "Performance", -100, 100, 0, false},
    {"Portamento", "Performance", 0, 1000, 0, false},
    {"Master", "Output", -36, 6, -6, false},
    {"Clock", "Hardware", 0, 1, 0, true},
    {"Hardware envelope", "Hardware/Envelope", 0, 1, 0, true},
    {"Envelope rate", "Hardware/Envelope", 0, 15, 8, true},
    {"Sweep depth", "Hardware/Sweep", -24, 24, 0, false},
    {"Sweep time", "Hardware/Sweep", 1, 1000, 120, false},
    {"Arpeggio", "Sequences", 0, 5, 0, true},
    {"Arpeggio rate", "Sequences", 1, 60, 12, false},
    {"DPCM rate", "NES/DPCM", 0, 15, 12, true},
    {"FM algorithm", "FM synthesis", 0, 31, 0, true},
    {"FM feedback", "Genesis/YM2612", 0, 7, 3, false},
    {"Retro amount", "Output/Console and TV", 0, 1, 0, false},
    {"Bit depth", "Output/Console and TV", 4, 16, 16, true},
    {"Output rate", "Output/Console and TV", 4000, 48000, 48000, false},
    {"RF noise", "Output/Console and TV", 0, 1, 0, false},
    {"Mains hum", "Output/Console and TV", 0, 1, 0, false},
    {"TV speaker", "Output/Console and TV", 0, 1, 0, false},
    {"Stereo width", "Output", 0, 1, 0, false},
    {"Chip cutoff", "Chip filter", 40, 16000, 6000, false},
    {"Chip resonance", "Chip filter", 0, 1, 0.25, false},
    {"Table position", "Wavetable synthesis", 0, 1, 0, false},
    {"Table warp", "Wavetable synthesis", 0, 1, 0.5, false},
    {"Harmonic tilt", "Additive synthesis", 0, 1, 0.45, false},
    {"FM brightness", "FM synthesis", 0, 1, 0.65, false},
    {"Layer", "Voice stacking", 0, 5, 0, true},
    {"Layer mix", "Voice stacking", 0, 1, 0.35, false},
    {"Vibrato rate", "Performance", 0.1, 20, 5.5, false},
    {"Vibrato depth", "Performance", 0, 2, 0, false},
    {"Drive", "Effects/Retro rack", 0, 1, 0, false},
    {"Echo mix", "Effects/Retro rack", 0, 1, 0, false},
    {"Echo time", "Effects/Retro rack", 10, 1000, 180, false},
    {"Echo feedback", "Effects/Retro rack", 0, 0.92, 0.35, false},
    {"Chorus mix", "Effects/Retro rack", 0, 1, 0, false},
    {"Chorus rate", "Effects/Retro rack", 0.05, 8, 0.8, false},
    {"Chorus depth", "Effects/Retro rack", 0, 12, 4, false},
    {"Tempo sync", "Composition", 0, 1, 0, true},
    {"Sync division", "Composition", 0, 7, 3, true},
    {"Strict hardware", "Composition", 0, 1, 0, true},
    {"Sequence length", "Sequences/User", 1, 8, 4, true},
    {"Step 1", "Sequences/User", -24, 24, 0, true},
    {"Step 2", "Sequences/User", -24, 24, 4, true},
    {"Step 3", "Sequences/User", -24, 24, 7, true},
    {"Step 4", "Sequences/User", -24, 24, 12, true},
    {"Step 5", "Sequences/User", -24, 24, 0, true},
    {"Step 6", "Sequences/User", -24, 24, 0, true},
    {"Step 7", "Sequences/User", -24, 24, 0, true},
    {"Step 8", "Sequences/User", -24, 24, 0, true},
    {"FM attack", "FM synthesis/Operators", 0, 31, 31, true},
    {"FM decay", "FM synthesis/Operators", 0, 31, 10, true},
    {"FM sustain rate", "FM synthesis/Operators", 0, 31, 5, true},
    {"FM sustain level", "FM synthesis/Operators", 0, 15, 2, true},
    {"FM release", "FM synthesis/Operators", 0, 15, 6, true},
    {"FM detune", "FM synthesis/Operators", 0, 7, 0, true},
    {"FM key scale", "FM synthesis/Operators", 0, 3, 1, true},
    {"FM LFO rate", "FM synthesis/LFO", 0, 7, 3, true},
    {"FM AM depth", "FM synthesis/LFO", 0, 127, 0, true},
    {"FM PM depth", "FM synthesis/LFO", 0, 127, 0, true},
    {"DPCM base key", "NES/DPCM bank", 0, 112, 36, true},
    {"DPCM loop mask", "NES/DPCM bank", 0, 65535, 0, true},
    {"DPCM initial level", "NES/DPCM bank", 0, 127, 64, true},
    {"DPCM trim start", "NES/DPCM bank", 0, 0.95, 0, false},
    {"DPCM trim end", "NES/DPCM bank", 0.05, 1, 1, false},
    {"Channel mute mask", "Hardware/Stack mixer", 0, 65535, 0, true},
    {"Channel solo mask", "Hardware/Stack mixer", 0, 65535, 0, true},
    {"Preset", "Presets", 0, 54, 0, true},
    {"Pitch bend range", "Performance", 0, 48, 2, true},
    {"Duty sequence", "Sequences/Duty", 0, 2, 0, true},
    {"Duty length", "Sequences/Duty", 1, 8, 4, true},
    {"Duty step rate", "Sequences/Duty", 1, 60, 15, false},
    {"Duty step 1", "Sequences/Duty", 0, 3, 0, true},
    {"Duty step 2", "Sequences/Duty", 0, 3, 1, true},
    {"Duty step 3", "Sequences/Duty", 0, 3, 2, true},
    {"Duty step 4", "Sequences/Duty", 0, 3, 3, true},
    {"Duty step 5", "Sequences/Duty", 0, 3, 3, true},
    {"Duty step 6", "Sequences/Duty", 0, 3, 2, true},
    {"Duty step 7", "Sequences/Duty", 0, 3, 1, true},
    {"Duty step 8", "Sequences/Duty", 0, 3, 0, true},
    {"Vibrato delay", "Performance", 0, 3000, 0, false},
    {"Cents sequence", "Sequences/Cents", 0, 2, 0, true},
    {"Cents length", "Sequences/Cents", 1, 8, 4, true},
    {"Cents step rate", "Sequences/Cents", 1, 60, 15, false},
    {"Cents step 1", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 2", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 3", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 4", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 5", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 6", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 7", "Sequences/Cents", -100, 100, 0, true},
    {"Cents step 8", "Sequences/Cents", -100, 100, 0, true},
}};

constexpr const char* kWaveNames[] = {
    "NES pulse", "NES triangle", "NES noise", "VRC6 pulse", "VRC6 saw",
    "FDS wavetable", "Namco 163 wavetable", "VRC7 FM", "Sunsoft 5B tone",
    "NES DPCM drums", "Game Boy pulse", "Game Boy wave", "Game Boy noise",
    "SMS tone", "SMS noise", "Genesis PSG tone", "Genesis PSG noise", "Genesis YM2612 FM",
    "NES five-channel stack", "Game Boy four-channel stack", "SMS four-channel stack",
    "Genesis ten-channel stack", "AY-3-8910 tone", "AY-3-8910 noise", "Atari POKEY tone",
    "Atari POKEY poly noise", "PC Engine wavetable", "OPL2 two-operator FM",
    "OPL3 four-operator FM", "Yamaha OPN/OPNA FM", "Yamaha OPM FM",
    "PC-88 YM2203 stack", "PC-98 YM2608 stack", "X68000 YM2151 stack",
    "Atari four-channel stack", "PC Engine six-channel stack", "Sound Blaster OPL3 stack",
    "PC Engine noise", "SID 6581", "SID 8580", "Konami SCC wavetable",
    "Konami SCC five-channel stack", "Philips SAA1099 tone", "SAA1099 six-channel stack",
    "Atari TIA polynomial tone", "Atari TIA two-channel stack", "Morphing wavetable",
    "Phase distortion", "Harmonic additive", "Six-operator FM", "Digital partial pair",
    "Porta FM keyboard", "Vintage analog poly", "Matrix brass poly", "Early digital ensemble",
    "Electromechanical tine", "Ladder mono synth", "Retro chip drum kit"};
constexpr const char* kArpNames[] = {"Off", "Major", "Minor", "Octaves", "NES chord", "User steps"};
constexpr const char* kLayerNames[] = {"Off", "Octave", "Fifth", "Sub octave", "Triangle", "Noise"};
constexpr const char* kPresetNames[] = {"Manual", "Clean NES lead", "NES chord lead",
    "NES DPCM kit", "Game Boy wave", "SMS bass", "Genesis FM bell",
    "Bedroom CRT", "Noisy RF television", "PC Engine glass", "DOS OPL2 organ",
    "OPL3 brass", "PC-88 adventure", "PC-98 FM piano", "X68000 arcade", "Atari POKEY zap",
    "SID 6581 bass", "SID 8580 lead", "Konami SCC lead", "Game Blaster bells",
    "Vector wavetable pad", "Phase-distortion brass", "Additive drawbars",
    "Six-operator electric piano", "Digital partial strings", "Envelope bass trick",
    "Hyper arpeggio lead", "Duty-cycle lead", "Fake echo lead", "Octave power bass",
    "Worn chorus pad", "VRC6 heroic lead", "FDS glass organ", "N163 ensemble",
    "YM2612 growl bass", "YM2151 arcade bell", "POKEY metallic zap",
    "SID combined reed", "DPCM sixteen-key bank", "User-sequence spark",
    "Arcade CRT cabinet", "Porta FM electric piano", "Porta FM toy organ",
    "Japanese analog poly", "American matrix brass", "Early sampler choir",
    "Tine suitcase piano", "Classic ladder bass", "Retro chip drum kit",
    "Game Boy bubble bloop", "Game Boy coin chirp", "Game Boy 7-bit zap",
    "LSDJ wave pluck", "Game Boy fast chord", "Game Boy tracker delay"};
constexpr const char* kDutyNames[] = {"12.5%", "25%", "50%", "75%"};
constexpr const char* kDutySeqNames[] = {"Off", "Loop", "One shot"};
// Steps per beat for each sync division, and how they read.
constexpr const char* kSyncDivisionNames[] = {"1 per bar", "1 per half", "1 per beat", "8ths", "8th triplets", "16ths", "16th triplets", "32nds"};
constexpr double kDuties[] = {0.125, 0.25, 0.5, 0.75};

inline bool format_value(uint32_t id, double value, char* text, uint32_t capacity) {
  if (id >= kParamCount || !text || capacity == 0) return false;
  if (id == kWaveform) std::snprintf(text, capacity, "%s", kWaveNames[std::clamp(static_cast<int>(std::round(value)), 0, 57)]);
  else if (id == kDuty || (id >= kDutyStep1 && id <= kDutyStep8)) std::snprintf(text, capacity, "%s", kDutyNames[std::clamp(static_cast<int>(std::round(value)), 0, 3)]);
  else if (id == kNoiseMode || id == kVelocity || id == kHardwareEnvelope || id == kTempoSync || id == kStrictHardware) std::snprintf(text, capacity, "%s", value >= 0.5 ? "On" : "Off");
  else if (id == kClockMode) std::snprintf(text, capacity, "%s", value >= 0.5 ? "PAL / 50 Hz" : "NTSC / 60 Hz");
  else if (id == kArpMode) std::snprintf(text, capacity, "%s", kArpNames[std::clamp(static_cast<int>(std::round(value)), 0, 5)]);
  else if (id == kLayerMode) std::snprintf(text, capacity, "%s", kLayerNames[std::clamp(static_cast<int>(std::round(value)), 0, 5)]);
  else if (id == kPreset) std::snprintf(text, capacity, "%s", kPresetNames[std::clamp(static_cast<int>(std::round(value)), 0, static_cast<int>(sizeof(kPresetNames) / sizeof(kPresetNames[0]) - 1))]);
  else if (id == kAttackMs || id == kReleaseMs || id == kPortamentoMs || id == kVibratoDelay || id == kEchoTime || id == kChorusDepth) std::snprintf(text, capacity, "%.1f ms", value);
  else if (id == kGainDb || id == kMasterDb) std::snprintf(text, capacity, "%.1f dB", value);
  else if (id == kFineTune) std::snprintf(text, capacity, "%.1f cents", value);
  else if (id >= kCentsStep1 && id <= kCentsStep8) std::snprintf(text, capacity, "%+.0f cents", value);
  else if (id == kFmRatio) std::snprintf(text, capacity, "%.2f : 1", value);
  else if (id == kFmIndex || id == kGenesisFeedback || id == kRetroAmount || id == kRfNoise || id == kHum || id == kSpeaker || id == kStereoWidth || id == kChipResonance || id == kWavetablePosition || id == kWavetableWarp || id == kAdditiveTilt || id == kFmBrightness || id == kLayerMix || id == kDrive || id == kEchoMix || id == kEchoFeedback || id == kChorusMix) std::snprintf(text, capacity, "%.2f", value);
  else if (id == kVibratoRate || id == kChorusRate) std::snprintf(text, capacity, "%.2f Hz", value);
  else if (id == kVibratoDepth) std::snprintf(text, capacity, "%.2f semitones", value);
  else if (id == kDutySeqMode || id == kCentsSeqMode) std::snprintf(text, capacity, "%s", kDutySeqNames[std::clamp(static_cast<int>(std::round(value)), 0, 2)]);
  else if (id == kDutySeqRate || id == kCentsSeqRate || id == kArpRate) std::snprintf(text, capacity, "%.1f steps/s", value);
  else if (id == kSyncDivision) std::snprintf(text, capacity, "%s", kSyncDivisionNames[std::clamp(static_cast<int>(std::round(value)), 0, 7)]);
  else if (id == kDpcmTrimStart || id == kDpcmTrimEnd) std::snprintf(text, capacity, "%.0f%%", value * 100.0);
  else if (id == kDpcmBaseKey) {
    static constexpr const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int key = std::clamp(static_cast<int>(std::lround(value)), 0, 127);
    std::snprintf(text, capacity, "%s%d (%d)", names[key % 12], key / 12 - 1, key);
  }
  else if (id == kPitchBendRange) std::snprintf(text, capacity, "%.0f semitones", value);
  else if (id == kChipCutoff) std::snprintf(text, capacity, "%.0f Hz", value);
  else if (id == kOutputRate) std::snprintf(text, capacity, "%.0f Hz", value);
  else std::snprintf(text, capacity, "%.0f", value);
  return true;
}
// Accepts anything format_value produces (so a host can round-trip its own display text),
// plus plain numbers in the parameter's native units.
inline bool parse_value(uint32_t id, const char* text, double* value) {
  if (id >= kParamCount || !text || !value) return false;
  const ParamSpec& spec = kSpecs[id];
  if (spec.stepped && spec.max - spec.min <= 128.0) {
    char candidate[128]{};
    for (double v = spec.min; v <= spec.max; v += 1.0) {
      format_value(id, v, candidate, sizeof(candidate));
      if (std::string_view(candidate) == text) { *value = v; return true; }
    }
  }
  const char* number = text;
  if (id == kDpcmBaseKey) if (const char* open = std::strchr(text, '(')) number = open + 1;
  char* end = nullptr;
  double parsed = std::strtod(number, &end);
  if (end == number || !std::isfinite(parsed)) return false;
  if ((id == kDpcmTrimStart || id == kDpcmTrimEnd) && std::strchr(end, '%')) parsed /= 100.0;
  *value = std::clamp(parsed, spec.min, spec.max);
  return true;
}

}  // namespace yanes::params
