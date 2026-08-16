#include <clap/clap.h>

// Every supported platform now carries the editor: the drawing and hit-testing are written once
// against yanes::ui::Canvas, and each platform below supplies that canvas plus a window backend.
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#define YANES_HAS_EDITOR 1
#endif

#ifdef YANES_HAS_EDITOR
#include "ui_canvas.hpp"
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include "ui_canvas_x11.hpp"
#elif defined(_WIN32)
#include "ui_canvas_win32.hpp"
#include <commdlg.h>
#elif defined(__APPLE__)
#include "ui_canvas_cocoa.hpp"
#endif
#endif

#include "dsp.hpp"
#include "hardware_fm.hpp"
#include "ui_layout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <new>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(YANES_HAS_EDITOR) && defined(__APPLE__)
// The Objective-C runtime keeps one flat, process-wide class namespace, so a plain "YanesEditorView"
// would collide with any other loaded copy of this plug-in. Version the name so a host that has two
// generations of YANES scanned at once still gets the view each bundle was compiled against.
#define YANES_EDITOR_VIEW YanesEditorView_0_2_0
// Only the declaration lives here; the implementation is at the end of the file, where the editor's
// drawing and input entry points are already in scope.
@interface YANES_EDITOR_VIEW : NSView
@property(assign, nonatomic) void* plugin;
@property(assign, nonatomic) NSTimer* refresh;
@end
#endif

namespace {

enum ParamId : clap_id {
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
  kPreset, kParamCount
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
    {"Preset", "Presets", 0, 48, 0, true},
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
    "Tine suitcase piano", "Classic ladder bass", "Retro chip drum kit"};
constexpr const char* kDutyNames[] = {"12.5%", "25%", "50%", "75%"};
constexpr double kDuties[] = {0.125, 0.25, 0.5, 0.75};

// Apple's libc++ still does not implement std::atomic<std::shared_ptr<T>> (P0718R2), so the DPCM
// bank slots go through this wrapper: the standard specialisation where the library has it, and a
// spinlock-guarded shared_ptr everywhere else. A slot is only written when the user loads or clears
// a bank and only read when a voice starts, so the fallback lock is uncontended in practice.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
template <typename T> using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;
#else
template <typename T> class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

  std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
    const Guard guard(lock_);
    return value_;
  }
  std::shared_ptr<T> exchange(std::shared_ptr<T> next, std::memory_order = std::memory_order_seq_cst) {
    const Guard guard(lock_);
    value_.swap(next);
    return next;
  }

 private:
  class Guard {
   public:
    explicit Guard(std::atomic_flag& flag) : flag_(flag) {
      while (flag_.test_and_set(std::memory_order_acquire)) {}
    }
    ~Guard() { flag_.clear(std::memory_order_release); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    std::atomic_flag& flag_;
  };

  mutable std::atomic_flag lock_{};
  std::shared_ptr<T> value_{};
};
#endif

struct Voice {
  bool active{};
  bool releasing{};
  int16_t channel{};
  int16_t key{-1};
  int32_t note_id{-1};
  double phase{};
  double note{};
  double target_note{};
  double env{};
  double velocity{};
  double noise_phase{};
  float noise_value{};
  yanes::NoiseLfsr lfsr{};
  uint64_t age{};
  uint64_t samples{};
  double dpcm_phase{};
  double dpcm_position{};
  size_t dpcm_bit{};
  uint8_t dpcm_slot{};
  int dpcm_level{64};
  uint32_t console_lfsr{1};
  uint32_t pokey_poly4{0x0f}, pokey_poly5{0x1f}, pokey_poly9{0x1ff}, pokey_poly17{0x1ffff};
  double chip_lp{}, chip_bp{};
  double fds_lp{};
  double layer_phase{};
  double aux_phase{};
  double tuning_expression{};
  double volume_expression{1.0};
  double brightness_expression{0.5};
  double pressure_expression{};
  bool sustained{};
  int hardware_signature{-1};
  int16_t port_index{};
  std::shared_ptr<const std::vector<uint8_t>> dpcm_data{};
};

struct Plugin {
  clap_plugin_t api{};
  const clap_host_t* host{};
  bool initialized{false};
  std::array<std::atomic<double>, kParamCount> params{};
  std::array<Voice, 16> voices{};
  std::array<yanes::HardwareFmVoice, 16> hardware_fm{};
  double sample_rate{48000.0};
  double previous_note{60.0};
  uint64_t age_counter{};
  uint32_t effect_rng{0x12345678U};
  double held_sample{};
  double hold_phase{};
  double hp_x{}, hp_y{}, lp_y{};
  double output_dc_x_l{}, output_dc_y_l{}, output_dc_x_r{}, output_dc_y_r{};
  std::atomic<float> output_peak_l{}, output_peak_r{};
  std::atomic<bool> output_clipped{};
  std::atomic<bool> params_rescan_pending{};
  double hum_phase{};
  double pitch_bend{};
  double mod_wheel{};
  std::vector<float> delay_buffer{};
  size_t delay_write{};
  double chorus_phase{};
  double tempo{120.0};
  std::array<AtomicSharedPtr<const std::vector<uint8_t>>, 16> dpcm_banks{};
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> retired_dpcm_banks{};
  std::array<std::atomic<float>,256> scope_samples{};
  std::atomic<uint32_t> scope_write{};
  std::atomic<uint64_t> scope_revision{};
  uint32_t scope_decimator{};
  // Touched by the audio thread and by parameter changes on every platform, so these live outside
  // the editor blocks below: process() and params_flush() run whether or not an editor is open.
  struct GuiOutEvent { uint8_t type; clap_id id; double value; };
  static constexpr uint32_t kGuiOutCap = 128;
  static constexpr uint8_t kGuiBegin = 0, kGuiValue = 1, kGuiEnd = 2;
  std::array<GuiOutEvent, kGuiOutCap> gui_out{};
  std::atomic<uint32_t> gui_out_w{};
  std::atomic<uint32_t> gui_out_r{};
  std::atomic<uint64_t> fm_revision{1};
  uint64_t applied_fm_revision{};
  std::atomic<uint64_t> ui_revision{1};
#ifdef YANES_HAS_EDITOR
  // Editor state that has nothing to do with the window system, shared by all three backends.
  uint32_t gui_width{yanes::ui::width}, gui_height{yanes::ui::height};
  int gui_page{};
  uint64_t gui_seen_revision{};
  uint64_t gui_seen_scope_revision{};
  unsigned gui_scope_ticks{};
  int gui_hover_param{-1};
  int gui_hover_tab{-1};
  int gui_drag_param{-1};
#if defined(__linux__)
  Display* display{};
  Window window{};
  GC gc{};
  XftDraw* gui_xft_draw{};
  XftFont* gui_xft_font{};
  int gui_font_pixels{};
  // The editor used to run its X11 loop on a thread of its own, which put painting,
  // parameter edits and host callbacks on a thread CLAP reserves for the host's main
  // thread. Everything below drives the same loop from the host instead: the connection's
  // descriptor wakes us for X events and the timer covers idle repaints, so the editor,
  // the parameter writes it makes and the host callbacks that follow all stay main-thread.
  const clap_host_timer_support_t* host_timers{};
  const clap_host_posix_fd_support_t* host_fds{};
  clap_id gui_timer{CLAP_INVALID_ID};
  int gui_fd{-1};
  // A file dialog is a separate process; it is collected from the timer rather than waited
  // on, so an open dialog never holds up the host.
  FILE* gui_picker{};
  int gui_picker_slot{-1};
  std::string gui_picker_output{};
#elif defined(_WIN32)
  HWND hwnd{};
#elif defined(__APPLE__)
  YANES_EDITOR_VIEW* view{};
#endif
#endif
  std::array<bool,16> sustain_pedal{};
};

Plugin* self(const clap_plugin_t* plugin) { return static_cast<Plugin*>(plugin->plugin_data); }
void install_dpcm_bank(Plugin* p,size_t slot,std::shared_ptr<const std::vector<uint8_t>> bank){
  p->retired_dpcm_banks.erase(std::remove_if(p->retired_dpcm_banks.begin(),p->retired_dpcm_banks.end(),
    [](const auto& owner){return owner.use_count()==1;}),p->retired_dpcm_banks.end());
  auto old=p->dpcm_banks[slot].exchange(std::move(bank),std::memory_order_acq_rel);if(old)p->retired_dpcm_banks.push_back(std::move(old));
}
std::shared_ptr<const std::vector<uint8_t>> load_dpcm_file(const std::string& path){
  std::ifstream input(path,std::ios::binary);if(!input)return {};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});if(bytes.empty()||bytes.size()>1024U*1024U)return {};
  const bool riff=bytes.size()>=4&&!std::memcmp(bytes.data(),"RIFF",4);
  if(!riff){
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
  }
  if(bytes.size()<44||std::memcmp(bytes.data()+8,"WAVE",4))return {};
  auto u16=[&](size_t p){return static_cast<uint16_t>(bytes[p]|(bytes[p+1]<<8U));};
  auto u32=[&](size_t p){return static_cast<uint32_t>(u16(p)|(static_cast<uint32_t>(u16(p+2))<<16U));};
  uint16_t format=0,channels=0,bits=0;uint32_t rate=0;size_t at=0,size=0;
  for(size_t p=12;p+8<=bytes.size();){const uint32_t n=u32(p+4);if(p+8U+n>bytes.size())break;if(!std::memcmp(bytes.data()+p,"fmt ",4)&&n>=16){format=u16(p+8);channels=u16(p+10);rate=u32(p+12);bits=u16(p+22);}else if(!std::memcmp(bytes.data()+p,"data",4)){at=p+8;size=n;}p+=8U+n+(n&1U);}
  if(format!=1||channels<1||channels>2||bits!=16||!rate||size<2U*channels)return {};
  constexpr double target_rate=16744.0;const size_t frames=size/(2U*channels),out_bits=static_cast<size_t>(static_cast<double>(frames)*target_rate/rate);
  if(!out_bits||out_bits>8U*1024U*1024U)return {};
  std::vector<uint8_t> encoded((out_bits+7U)/8U);int level=64;
  for(size_t i=0;i<out_bits;++i){const size_t frame=std::min(frames-1U,static_cast<size_t>(static_cast<double>(i)*rate/target_rate));int sum=0;for(uint16_t ch=0;ch<channels;++ch){const size_t q=at+(frame*channels+ch)*2U;sum+=static_cast<int16_t>(u16(q));}const int target=std::clamp(64+(sum/channels)*60/32768,0,127);const bool up=target>=level;level=std::clamp(level+(up?2:-2),0,127);if(up)encoded[i>>3U]|=static_cast<uint8_t>(1U<<(i&7U));}
  return std::make_shared<const std::vector<uint8_t>>(std::move(encoded));
}
double db_gain(double db) { return std::exp(db * std::log(10.0) / 20.0); }

// Picking a chip voice has to sound like that chip straight away, with no further
// tweaking. These are the register states each chip comes up in — the same ones
// Furnace's default instrument plays, which is what makes the audio parity suite
// a test of what a user actually hears rather than of settings only the test
// knows about. A value of -1 leaves the parameter wherever it already was.
struct VoiceDefaults {
  int waveform;
  double duty, shape, noise_period, noise_mode, fm_ratio, fm_index, release_ms;
};
// Every one of these chips silences its channel the moment the gate clears, so a
// release of 0 is the authentic tail, not an omission — the exceptions are the
// SIDs, which run their own envelope generator past the gate.
constexpr VoiceDefaults kVoiceDefaults[] = {
    //  wave  duty shape period mode ratio index release
    {0, 0, -1, -1, -1, -1, -1, 0},     // NES pulse: 12.5% duty
    {1, -1, -1, -1, -1, -1, -1, 0},    // NES triangle
    {2, -1, -1, 15, 1, -1, -1, 0},     // NES noise: longest period, short mode
    {3, -1, 0, -1, -1, -1, -1, 0},     // VRC6 pulse: narrowest duty
    {4, -1, 7, -1, -1, -1, -1, 0},     // VRC6 saw: full accumulator rate
    {5, -1, 7, -1, -1, -1, -1, 0},     // FDS: the ramp the wavetable holds at reset
    {6, -1, 7, -1, -1, -1, -1, 0},     // Namco 163: ditto
    {7, -1, -1, -1, -1, 1, 4, 0},      // VRC7: OPLL patch, modulator at the carrier
    {10, 0, -1, -1, -1, -1, -1, 0},    // Game Boy pulse: 12.5% duty
    {11, -1, 7, -1, -1, -1, -1, 0},    // Game Boy wave: reset ramp
    {12, -1, -1, -1, -1, -1, -1, 0},   // Game Boy noise
    {13, -1, -1, -1, -1, -1, -1, 0},   // SMS tone
    {14, -1, -1, -1, 1, -1, -1, 0},    // SMS noise: white
    {15, -1, -1, -1, -1, -1, -1, 0},   // Genesis PSG is the same SN76489
    {16, -1, -1, -1, 1, -1, -1, 0},    // Genesis PSG noise: white
    {22, -1, -1, -1, -1, -1, -1, 0},   // AY-3-8910 tone
    {24, -1, 0, -1, -1, -1, -1, 0},    // POKEY: pure tone
    {26, -1, 7, -1, -1, -1, -1, 0},    // PC Engine: reset ramp
    {38, -1, -1, -1, -1, -1, -1, 172}, // SID 6581 envelope release
    {39, -1, -1, -1, -1, -1, -1, 92},  // SID 8580 envelope release
    {40, -1, 7, -1, -1, -1, -1, 0},    // Konami SCC: reset ramp
    {42, -1, -1, -1, -1, -1, -1, 0},   // Philips SAA1099 tone
    {44, -1, 0, -1, -1, -1, -1, 0},    // TIA: pure tone
};

void set_param(Plugin* p, clap_id id, double value, bool apply_preset);

void apply_voice_defaults(Plugin* p, int waveform) {
  for (const auto& voice : kVoiceDefaults) {
    if (voice.waveform != waveform) continue;
    const std::pair<clap_id, double> settings[] = {
        {kDuty, voice.duty},           {kExpansionShape, voice.shape},
        {kNoisePeriod, voice.noise_period}, {kNoiseMode, voice.noise_mode},
        {kFmRatio, voice.fm_ratio},    {kFmIndex, voice.fm_index},
        {kReleaseMs, voice.release_ms}};
    for (const auto& [target, setting] : settings)
      if (setting >= 0.0) set_param(p, target, setting, false);
    // The voice brought several parameters with it, so the host has to re-read
    // them or its panel and automation lanes keep showing the old voice's values.
    if (p->initialized) {
      p->params_rescan_pending.store(true, std::memory_order_release);
      if (p->host && p->host->request_callback) p->host->request_callback(p->host);
    }
    return;
  }
}

void set_param(Plugin* p, clap_id id, double value, bool apply_preset = true) {
  if (id >= kParamCount) return;
  const auto& s = kSpecs[id];
  value = std::clamp(value, s.min, s.max);
  if (s.stepped) value = std::round(value);
  const double previous=p->params[id].exchange(value,std::memory_order_relaxed);
  if(previous!=value)p->ui_revision.fetch_add(1,std::memory_order_release);
  if (id == kGenesisAlgorithm || id == kGenesisFeedback || id == kFmBrightness ||
      (id >= kFmAttack && id <= kFmPmDepth)) p->fm_revision.fetch_add(1, std::memory_order_release);
  // Only on an actual change, so re-sending the current voice never overwrites
  // edits the player has made on top of it.
  if (id == kWaveform && previous != value) apply_voice_defaults(p, static_cast<int>(value));
  if (!apply_preset || id != kPreset) return;
  // A preset is a complete recipe, so every other parameter returns to its default before the
  // recipe runs. Without this, selecting a preset only layered its own edits on top of whatever
  // the previous one left behind: leaving a preset that enables console noise, an arpeggio, or an
  // effect kept that setting audible under every preset chosen afterwards.
  // Master is the user's output level rather than part of any recipe, so it survives the reset.
  for (clap_id target = 0; target < kParamCount; ++target)
    if (target != kPreset && target != kMasterDb) set_param(p, target, kSpecs[target].def, false);
  apply_voice_defaults(p, static_cast<int>(kSpecs[kWaveform].def));
  // Preset recipes change many parameters at once; ask the host to re-read them all so its
  // generic panel and automation lanes do not keep showing the previous preset's values.
  if (p->initialized) {
    p->params_rescan_pending.store(true, std::memory_order_release);
    if (p->host && p->host->request_callback) p->host->request_callback(p->host);
  }
  auto put = [p](clap_id target, double v) { set_param(p, target, v, false); };
  // Presets only touch their relevant synthesis/output sections so they remain useful starting points.
  switch (static_cast<int>(value)) {
    case 1: put(kWaveform, 0); put(kDuty, 1); put(kArpMode, 0); put(kRetroAmount, 0); break;
    case 2: put(kWaveform, 0); put(kDuty, 2); put(kArpMode, 4); put(kArpRate, 18); break;
    case 3: put(kWaveform, 9); put(kDpcmRate, 12); put(kReleaseMs, 40); break;
    case 4: put(kWaveform, 11); put(kHardwareEnvelope, 1); put(kEnvelopeRate, 5); break;
    case 5: put(kWaveform, 13); put(kTranspose, -12); put(kAttackMs, 0); break;
    case 6: put(kWaveform, 17); put(kGenesisAlgorithm, 4); put(kGenesisFeedback, 4); put(kFmBrightness, 0.85); put(kReleaseMs, 650); break;
    case 7: put(kRetroAmount, 0.75); put(kSpeaker, 0.8); put(kOutputRate, 18000); put(kBitDepth, 11); break;
    case 8: put(kRetroAmount, 1); put(kSpeaker, 1); put(kOutputRate, 11000); put(kBitDepth, 8); put(kRfNoise, 0.35); put(kHum, 0.25); break;
    case 9: put(kWaveform, 26); put(kExpansionShape, 2); put(kReleaseMs, 300); break;
    case 10: put(kWaveform, 27); put(kFmRatio, 2); put(kFmIndex, 3.8); put(kGenesisFeedback, 2); put(kFmBrightness, 0.95); break;
    case 11: put(kWaveform, 28); put(kGenesisAlgorithm, 4); put(kGenesisFeedback, 5); put(kFmBrightness, 0.95); break;
    case 12: put(kWaveform, 31); put(kGenesisAlgorithm, 2); put(kFmBrightness, 0.95); put(kRetroAmount, 0.15); break;
    case 13: put(kWaveform, 32); put(kGenesisAlgorithm, 5); put(kReleaseMs, 450); break;
    case 14: put(kWaveform, 33); put(kGenesisAlgorithm, 1); put(kGenesisFeedback, 6); put(kFmBrightness, 0.95); break;
    case 15: put(kWaveform, 25); put(kNoiseMode, 1); put(kSweepDepth, 18); put(kSweepTime, 90); break;
    case 16: put(kWaveform, 38); put(kDuty, 1); put(kChipCutoff, 900); put(kChipResonance, 0.72); put(kTranspose, -12); break;
    case 17: put(kWaveform, 39); put(kDuty, 2); put(kChipCutoff, 5200); put(kChipResonance, 0.48); break;
    case 18: put(kWaveform, 40); put(kExpansionShape, 5); put(kReleaseMs, 120); break;
    case 19: put(kWaveform, 42); put(kArpMode, 1); put(kArpRate, 14); put(kStereoWidth, 1); break;
    case 20: put(kWaveform, 46); put(kWavetablePosition, 0.35); put(kWavetableWarp, 0.62); put(kAttackMs, 80); put(kReleaseMs, 900); break;
    case 21: put(kWaveform, 47); put(kWavetablePosition, 0.7); put(kWavetableWarp, 0.28); put(kChipCutoff, 4800); break;
    case 22: put(kWaveform, 48); put(kAdditiveTilt, 0.52); put(kWavetablePosition, 0.25); break;
    case 23: put(kWaveform, 49); put(kGenesisAlgorithm, 28); put(kFmIndex, 3.4); put(kFmBrightness, 0.58); put(kReleaseMs, 700); break;
    case 24: put(kWaveform, 50); put(kWavetablePosition, 0.42); put(kChipCutoff, 3400); put(kAttackMs, 35); put(kReleaseMs, 1200); break;
    case 25: put(kWaveform, 8); put(kHardwareEnvelope, 1); put(kEnvelopeRate, 14); put(kTranspose, -24); put(kDrive, 0.25); break;
    case 26: put(kWaveform, 0); put(kArpMode, 4); put(kArpRate, 30); put(kDuty, 1); put(kReleaseMs, 25); break;
    case 27: put(kWaveform, 0); put(kDuty, 0); put(kLayerMode, 4); put(kLayerMix, 0.28); put(kVibratoDepth, 0.12); break;
    case 28: put(kWaveform, 0); put(kEchoMix, 0.38); put(kEchoTime, 92); put(kEchoFeedback, 0.48); break;
    case 29: put(kWaveform, 8); put(kLayerMode, 1); put(kLayerMix, 0.34); put(kTranspose, -12); put(kDrive, 0.3); break;
    case 30: put(kWaveform, 46); put(kChorusMix, 0.52); put(kChorusRate, 0.42); put(kChorusDepth, 7); put(kRetroAmount, 0.25); break;
    case 31: put(kWaveform, 3); put(kExpansionShape, 6); put(kSweepDepth, -5); put(kSweepTime, 280); put(kVibratoDepth, 0.18); break;
    case 32: put(kWaveform, 5); put(kExpansionShape, 4); put(kAttackMs, 8); put(kReleaseMs, 420); put(kChorusMix, 0.18); break;
    case 33: put(kWaveform, 6); put(kExpansionShape, 5); put(kLayerMode, 2); put(kLayerMix, 0.16); put(kChorusMix, 0.34); break;
    case 34: put(kWaveform, 17); put(kGenesisAlgorithm, 4); put(kGenesisFeedback, 6); put(kFmDetune, 2); put(kFmAttack, 31); put(kFmDecay, 14); put(kFmSustainLevel, 5); put(kTranspose, -12); put(kDrive, 0.38); break;
    case 35: put(kWaveform, 30); put(kGenesisAlgorithm, 7); put(kGenesisFeedback, 3); put(kFmAttack, 28); put(kFmDecay, 12); put(kFmRelease, 8); put(kFmBrightness, 0.78); break;
    case 36: put(kWaveform, 25); put(kExpansionShape, 7); put(kNoiseMode, 1); put(kSweepDepth, 22); put(kSweepTime, 65); put(kReleaseMs, 80); break;
    case 37: put(kWaveform, 38); put(kExpansionShape, 5); put(kDuty, 1); put(kChipCutoff, 1800); put(kChipResonance, 0.66); put(kDrive, 0.24); break;
    case 38: put(kWaveform, 9); put(kDpcmBaseKey, 36); put(kDpcmRate, 15); put(kReleaseMs, 18); break;
    case 39: put(kWaveform, 0); put(kArpMode, 5); put(kSequenceLength, 6); put(kSequence1, 0); put(kSequence2, 7); put(kSequence3, 12); put(kSequence4, 4); put(kSequence5, 16); put(kSequence6, 11); put(kTempoSync, 1); break;
    case 40: put(kWaveform, 28); put(kRetroAmount, 0.58); put(kSpeaker, 0.75); put(kDrive, 0.22); put(kChorusMix, 0.16); put(kRfNoise, 0.06); put(kOutputRate, 22000); break;
    case 41: put(kWaveform,51);put(kFmRatio,3);put(kFmIndex,2.7);put(kFmBrightness,0.58);put(kAttackMs,3);put(kReleaseMs,620);put(kChorusMix,0.14);put(kOutputRate,32000);break;
    case 42: put(kWaveform,51);put(kFmRatio,2);put(kFmIndex,4.6);put(kFmBrightness,0.76);put(kAttackMs,1);put(kReleaseMs,180);put(kHardwareEnvelope,1);put(kEnvelopeRate,6);break;
    case 43: put(kWaveform,52);put(kExpansionShape,3);put(kChipCutoff,4200);put(kChipResonance,0.28);put(kAttackMs,22);put(kReleaseMs,740);put(kChorusMix,0.38);put(kChorusRate,0.31);break;
    case 44: put(kWaveform,53);put(kExpansionShape,6);put(kChipCutoff,2800);put(kChipResonance,0.52);put(kAttackMs,8);put(kReleaseMs,460);put(kLayerMode,2);put(kLayerMix,0.12);break;
    case 45: put(kWaveform,54);put(kWavetablePosition,0.62);put(kAttackMs,95);put(kReleaseMs,1500);put(kChorusMix,0.44);put(kChorusRate,0.24);put(kRetroAmount,0.12);break;
    case 46: put(kWaveform,55);put(kFmIndex,3.2);put(kFmBrightness,0.66);put(kAttackMs,2);put(kReleaseMs,920);put(kChorusMix,0.18);put(kDrive,0.08);break;
    case 47: put(kWaveform,56);put(kChipCutoff,820);put(kChipResonance,0.64);put(kTranspose,-12);put(kAttackMs,1);put(kReleaseMs,180);put(kDrive,0.32);break;
    case 48: put(kWaveform,57);put(kAttackMs,0);put(kReleaseMs,24);put(kExpansionShape,4);put(kVelocity,1);put(kTranspose,0);put(kFineTune,0);put(kArpMode,0);put(kLayerMode,0);put(kHardwareEnvelope,0);put(kDrive,0.12);put(kRetroAmount,0.08);break;
    default: break;
  }
}

yanes::FmControls fm_controls(const Plugin* p) {
  yanes::FmControls c;
  c.algorithm=static_cast<int>(p->params[kGenesisAlgorithm].load());c.feedback=static_cast<int>(p->params[kGenesisFeedback].load());
  c.attack=static_cast<int>(p->params[kFmAttack].load());c.decay=static_cast<int>(p->params[kFmDecay].load());c.sustain_rate=static_cast<int>(p->params[kFmSustainRate].load());
  c.sustain_level=static_cast<int>(p->params[kFmSustainLevel].load());c.release=static_cast<int>(p->params[kFmRelease].load());c.detune=static_cast<int>(p->params[kFmDetune].load());
  c.key_scale=static_cast<int>(p->params[kFmKeyScale].load());c.lfo_rate=static_cast<int>(p->params[kFmLfoRate].load());c.am_depth=static_cast<int>(p->params[kFmAmDepth].load());
  c.pm_depth=static_cast<int>(p->params[kFmPmDepth].load());c.brightness=p->params[kFmBrightness].load();return c;
}
yanes::HardwareFmVoice::Kind fm_kind(int waveform,int selected){
  if(waveform==27)return yanes::HardwareFmVoice::Kind::Opl2;
  if(waveform==28)return selected==36?yanes::HardwareFmVoice::Kind::Opl3:yanes::HardwareFmVoice::Kind::Opl3FourOp;
  if(waveform==29)return selected==31?yanes::HardwareFmVoice::Kind::Opn:yanes::HardwareFmVoice::Kind::Opna;
  if(waveform==30)return yanes::HardwareFmVoice::Kind::Opm;
  return yanes::HardwareFmVoice::Kind::Ym2612;
}

void gui_push(Plugin* p, uint8_t type, clap_id id, double value) {
  const uint32_t w = p->gui_out_w.load(std::memory_order_relaxed);
  const uint32_t r = p->gui_out_r.load(std::memory_order_acquire);
  if (w - r >= Plugin::kGuiOutCap) return;
  p->gui_out[w % Plugin::kGuiOutCap] = {type, id, value};
  p->gui_out_w.store(w + 1, std::memory_order_release);
}

void emit_gui_events(Plugin* p, const clap_output_events_t* out) {
  if (!out) return;
  uint32_t r = p->gui_out_r.load(std::memory_order_relaxed);
  const uint32_t w = p->gui_out_w.load(std::memory_order_acquire);
  while (r != w) {
    const auto event = p->gui_out[r % Plugin::kGuiOutCap];
    if (event.type == Plugin::kGuiBegin || event.type == Plugin::kGuiEnd) {
      clap_event_param_gesture_t g{};
      g.header.size = sizeof(g);
      g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      g.header.type = event.type == Plugin::kGuiBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                     : CLAP_EVENT_PARAM_GESTURE_END;
      g.param_id = event.id;
      out->try_push(out, &g.header);
    } else {
      clap_event_param_value_t v{};
      v.header.size = sizeof(v);
      v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      v.header.type = CLAP_EVENT_PARAM_VALUE;
      v.param_id = event.id;
      v.note_id = -1;
      v.port_index = -1;
      v.channel = -1;
      v.key = -1;
      v.value = event.value;
      out->try_push(out, &v.header);
    }
    ++r;
  }
  p->gui_out_r.store(r, std::memory_order_release);
}

void emit_note_end(const clap_output_events_t* out, uint32_t time, const Voice& v) {
  if (!out) return;
  clap_event_note_t e{};
  e.header.size = sizeof(e);
  e.header.time = time;
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = CLAP_EVENT_NOTE_END;
  e.note_id = v.note_id;
  e.port_index = v.port_index;
  e.channel = v.channel;
  e.key = v.key;
  out->try_push(out, &e.header);
}

void stop_hardware(Plugin* p, Voice& v) {
  p->hardware_fm[static_cast<size_t>(&v - p->voices.data())].key_off();
  v.hardware_signature = -1;
}

void kill_voice(Plugin* p, Voice& v, const clap_output_events_t* out, uint32_t time) {
  if (!v.active) return;
  stop_hardware(p, v);
  emit_note_end(out, time, v);
  v.active = false;
  v.releasing = false;
  v.sustained = false;
}

void release_voice(Plugin* p, Voice& v) {
  if (!v.active) return;
  stop_hardware(p, v);
  v.releasing = true;
  v.sustained = false;
}

bool matches(const Voice& v, const clap_event_note_t& e) {
  return v.active && (e.channel < 0 || v.channel == e.channel) &&
         (e.key < 0 || v.key == e.key) && (e.note_id < 0 || v.note_id == e.note_id);
}

void note_on(Plugin* p, int channel, int key, int note_id, double velocity, int16_t port,
             const clap_output_events_t* out, uint32_t time) {
  const int selected = static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed));
  const bool stack_mode = selected == 18 || selected == 19 || selected == 20 || selected == 21 ||
                          selected == 31 || selected == 32 || selected == 33 || selected == 34 ||
                          selected == 35 || selected == 36 || selected == 41 || selected == 43 || selected == 45;
  if (stack_mode && p->params[kStrictHardware].load(std::memory_order_relaxed) >= 0.5) {
    for (auto& v : p->voices) if (v.active && v.channel == channel) kill_voice(p, v, out, time);
  }
  Voice* voice = nullptr;
  for (auto& v : p->voices) if (!v.active) { voice = &v; break; }
  if (!voice) {
    voice = &*std::min_element(p->voices.begin(), p->voices.end(),
        [](const Voice& a, const Voice& b) { return a.age < b.age; });
    kill_voice(p, *voice, out, time);
  }
  const double glide = p->params[kPortamentoMs].load(std::memory_order_relaxed);
  *voice = Voice{};
  voice->active = true;
  voice->channel = static_cast<int16_t>(channel);
  voice->key = static_cast<int16_t>(key);
  voice->note_id = note_id;
  voice->port_index = port;
  voice->target_note = static_cast<double>(key);
  voice->note = glide > 0.0 ? p->previous_note : voice->target_note;
  voice->velocity = std::clamp(velocity, 0.0, 1.0);
  voice->lfsr.reset(static_cast<uint16_t>((++p->age_counter * 1103515245ULL) & 0x7fffU));
  voice->noise_value = voice->lfsr.clock(false);
  voice->age = p->age_counter;
  voice->dpcm_slot = static_cast<uint8_t>(std::clamp(key - static_cast<int>(p->params[kDpcmBaseKey].load()), 0, 15));
  voice->dpcm_data = p->dpcm_banks[voice->dpcm_slot].load(std::memory_order_acquire);
  if(voice->dpcm_data)voice->dpcm_bit=static_cast<size_t>(static_cast<double>(voice->dpcm_data->size()*8U)*p->params[kDpcmTrimStart].load());
  voice->dpcm_level = static_cast<int>(p->params[kDpcmInitialLevel].load());
  const size_t voice_index = static_cast<size_t>(voice - p->voices.data());
  const int selected_waveform = static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed));
  int fm_waveform = selected_waveform;
  if (selected_waveform == 21 && channel < 6) fm_waveform = 17;
  if ((selected_waveform == 31 && channel < 3) || (selected_waveform == 32 && channel < 6)) fm_waveform = 29;
  if (selected_waveform == 33 && channel < 8) fm_waveform = 30;
  if (selected_waveform == 36) fm_waveform = 28;
  if (fm_waveform == 17 || (fm_waveform >= 27 && fm_waveform <= 30)) {
    const auto kind=fm_kind(fm_waveform,selected_waveform);
    p->hardware_fm[voice_index].key_on(kind, yanes::midi_frequency(key), fm_controls(p));
    voice->hardware_signature=fm_waveform|(static_cast<int>(kind)<<8);
  }
  p->previous_note = voice->target_note;
}

void handle_event(Plugin* p, const clap_event_header_t* h, const clap_output_events_t* out, uint32_t time) {
  if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (h->type == CLAP_EVENT_PARAM_VALUE) {
    const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
    set_param(p, e->param_id, e->value);
  } else if (h->type == CLAP_EVENT_NOTE_ON) {
    const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
    note_on(p, e->channel, e->key, e->note_id, e->velocity, e->port_index, out, time);
  } else if (h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE) {
    const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
    for (auto& v : p->voices) if (matches(v, *e)) {
      if (h->type == CLAP_EVENT_NOTE_CHOKE) kill_voice(p, v, out, time);
      else if (v.channel>=0&&v.channel<16&&p->sustain_pedal[static_cast<size_t>(v.channel)]) v.sustained=true;
      else release_voice(p, v);
    }
  } else if (h->type == CLAP_EVENT_NOTE_EXPRESSION) {
    const auto* e = reinterpret_cast<const clap_event_note_expression_t*>(h);
    for (auto& v : p->voices) if (v.active &&
        (e->channel < 0 || v.channel == e->channel) && (e->key < 0 || v.key == e->key) &&
        (e->note_id < 0 || v.note_id == e->note_id) &&
        (e->port_index < 0 || v.port_index == e->port_index)) {
      if (e->expression_id == CLAP_NOTE_EXPRESSION_TUNING) v.tuning_expression = e->value;
      if (e->expression_id == CLAP_NOTE_EXPRESSION_VOLUME) v.volume_expression = std::max(0.0, e->value);
      if (e->expression_id == CLAP_NOTE_EXPRESSION_BRIGHTNESS) v.brightness_expression = std::clamp(e->value, 0.0, 1.0);
      if (e->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE) v.pressure_expression = std::clamp(e->value, 0.0, 1.0);
    }
  } else if (h->type == CLAP_EVENT_TRANSPORT) {
    const auto* e = reinterpret_cast<const clap_event_transport_t*>(h);
    if ((e->flags & CLAP_TRANSPORT_HAS_TEMPO) && e->tempo > 1.0) p->tempo = e->tempo;
  } else if (h->type == CLAP_EVENT_MIDI) {
    const auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
    const int status = e->data[0] & 0xf0;
    const int channel = e->data[0] & 0x0f;
    if (status == 0x90 && e->data[2] != 0) note_on(p, channel, e->data[1], -1, e->data[2] / 127.0, 0, out, time);
    if (status == 0x80 || (status == 0x90 && e->data[2] == 0)) {
      for (auto& v : p->voices) if (v.active && v.channel == channel && v.key == e->data[1]) {
        if(p->sustain_pedal[static_cast<size_t>(channel)])v.sustained=true;
        else release_voice(p, v);
      }
    }
    if (status == 0xb0 && e->data[1] == 1) p->mod_wheel = e->data[2] / 127.0;
    if (status == 0xb0 && e->data[1] == 64) {
      const bool down=e->data[2]>=64;p->sustain_pedal[static_cast<size_t>(channel)]=down;
      if(!down)for(auto& v:p->voices)if(v.active&&v.channel==channel&&v.sustained)release_voice(p, v);
    }
    if (status == 0xb0 && e->data[1] == 120) {
      for (auto& v : p->voices) if (v.active && v.channel == channel) kill_voice(p, v, out, time);
    }
    if (status == 0xb0 && e->data[1] == 123) {
      for (auto& v : p->voices) if (v.active && v.channel == channel) {
        if (p->sustain_pedal[static_cast<size_t>(channel)]) v.sustained = true;
        else release_voice(p, v);
      }
    }
    if (status == 0xe0) {
      const int bend = e->data[1] | (e->data[2] << 7);
      p->pitch_bend = (bend - 8192) / 8192.0 * 2.0;
    }
  }
}

float render_voice(Plugin* p, Voice& v) {
  const double attack = p->params[kAttackMs].load(std::memory_order_relaxed);
  double release = p->params[kReleaseMs].load(std::memory_order_relaxed);
  if(static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed))==7&&v.key<60&&release<1.0)release=58.0;
  if (v.releasing) {
    v.env -= 1.0 / (p->sample_rate * std::max(0.001, release * 0.001));
    if (v.env <= 0.0) { stop_hardware(p, v); v.active = false; return 0.0f; }
  } else {
    v.env = attack <= 0.0 ? 1.0 : std::min(1.0, v.env + 1.0 / (p->sample_rate * attack * 0.001));
  }

  const double glide = p->params[kPortamentoMs].load(std::memory_order_relaxed);
  if (glide <= 0.0) v.note = v.target_note;
  else {
    const double coefficient = 1.0 - std::exp(-1.0 / (p->sample_rate * glide * 0.001));
    v.note += (v.target_note - v.note) * coefficient;
  }
  const double transpose = p->params[kTranspose].load(std::memory_order_relaxed);
  const double fine = p->params[kFineTune].load(std::memory_order_relaxed) / 100.0;
  const int arp = static_cast<int>(p->params[kArpMode].load(std::memory_order_relaxed));
  double arp_rate = p->params[kArpRate].load(std::memory_order_relaxed);
  constexpr double divisions[] = {0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0};
  if (p->params[kTempoSync].load(std::memory_order_relaxed) >= 0.5) {
    const int division = static_cast<int>(p->params[kSyncDivision].load(std::memory_order_relaxed));
    arp_rate = p->tempo / 60.0 * divisions[std::clamp(division, 0, 7)];
  }
  const double elapsed_samples = static_cast<double>(v.samples);
  const int arp_step = static_cast<int>((elapsed_samples * arp_rate / p->sample_rate)) % 3;
  constexpr int arp_intervals[5][3] = {{0,0,0},{0,4,7},{0,3,7},{0,12,24},{0,3,8}};
  double sequence_pitch = 0.0;
  if (arp == 5) {
    const int length = static_cast<int>(p->params[kSequenceLength].load(std::memory_order_relaxed));
    const int step = static_cast<int>(elapsed_samples * arp_rate / p->sample_rate) % std::clamp(length, 1, 8);
    sequence_pitch = p->params[static_cast<clap_id>(kSequence1 + step)].load(std::memory_order_relaxed);
  } else {
    sequence_pitch = arp_intervals[std::clamp(arp, 0, 4)][arp_step];
  }
  const double sweep_depth = p->params[kSweepDepth].load(std::memory_order_relaxed);
  const double sweep_time = p->params[kSweepTime].load(std::memory_order_relaxed) * 0.001;
  sequence_pitch += sweep_depth * std::min(1.0, elapsed_samples / (p->sample_rate * sweep_time));
  const double vibrato_depth = p->params[kVibratoDepth].load(std::memory_order_relaxed) + p->mod_wheel * 0.75;
  const double vibrato = std::sin(6.28318530718 * elapsed_samples *
      p->params[kVibratoRate].load(std::memory_order_relaxed) / p->sample_rate) * vibrato_depth;
  double frequency = yanes::midi_frequency(v.note + transpose + fine + sequence_pitch +
                                            v.tuning_expression + p->pitch_bend + vibrato);
  const int selected_waveform=static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed));
  int waveform=selected_waveform;
  if (waveform == 18) { // NES: MIDI channels 1..5 = pulse 1, pulse 2, triangle, noise, DPCM.
    constexpr int map[] = {0, 0, 1, 2, 9}; waveform = map[std::clamp<int>(v.channel, 0, 4)];
  } else if (waveform == 19) { // Game Boy: pulse 1, pulse 2, wave, noise.
    constexpr int map[] = {10, 10, 11, 12}; waveform = map[std::clamp<int>(v.channel, 0, 3)];
  } else if (waveform == 20) { // SMS: three tones and noise.
    waveform = v.channel == 3 ? 14 : 13;
  } else if (waveform == 21) { // Genesis: six FM, three PSG tones, PSG noise.
    waveform = v.channel < 6 ? 17 : (v.channel < 9 ? 15 : 16);
  } else if (waveform == 31) { // PC-88: three OPN FM and three AY/SSG channels.
    waveform = v.channel < 3 ? 29 : 22;
  } else if (waveform == 32) { // PC-98 OPNA: six FM, three SSG, six rhythm, one ADPCM.
    waveform = v.channel < 6 ? 29 : (v.channel < 9 ? 22 : (v.channel < 15 ? 58 : 9));
  } else if (waveform == 33) { // X68000: eight YM2151/OPM channels plus ADPCM.
    waveform = v.channel < 8 ? 30 : 9;
  } else if (waveform == 34) { // POKEY: four channels, alternate tone and polynomial noise.
    waveform = (v.channel & 1) ? 25 : 24;
  } else if (waveform == 35) { // PC Engine: four wavetable plus two noise-capable voices.
    waveform = v.channel < 4 ? 26 : 37;
  } else if (waveform == 36) { // Sound Blaster OPL3: melodic channels.
    waveform = 28;
  } else if (waveform == 41) { // SCC: five wavetable voices.
    waveform = 40;
  } else if (waveform == 43) { // SAA1099: six tone voices with two shared noise generators.
    waveform = 42;
  } else if (waveform == 45) { // TIA: two independently controlled polynomial voices.
    waveform = 44;
  }
  const bool hardware_waveform=waveform==17||(waveform>=27&&waveform<=30);
  if(!hardware_waveform&&v.hardware_signature>=0){p->hardware_fm[static_cast<size_t>(&v-p->voices.data())].key_off();v.hardware_signature=-1;}
  const double chip_clock = p->params[kClockMode].load(std::memory_order_relaxed) >= 0.5
                                ? 1662607.0 : yanes::kCpuClock;
  if (waveform == 0 || waveform == 1) {
    const double divider = waveform == 1 ? 32.0 : 16.0;
    const double timer = std::clamp(std::round(chip_clock / (divider * frequency) - 1.0), 0.0, 2047.0);
    frequency = chip_clock / (divider * (timer + 1.0));
  } else if (waveform == 10 || waveform == 11) {
    const double period = std::clamp(std::round(2048.0 - 131072.0 / std::max(8.0, frequency)), 0.0, 2047.0);
    frequency = 131072.0 / std::max(1.0, 2048.0 - period);
  } else if (waveform == 13 || waveform == 14 || waveform == 15 || waveform == 16) {
    // The noise channel runs off the third tone generator's period, so it gets
    // the same register quantisation as a tone.
    constexpr double sms_clock = 3579545.0;
    const double period = std::clamp(std::round(sms_clock / (32.0 * frequency)), 1.0, 1023.0);
    frequency = sms_clock / (32.0 * period);
  } else if (waveform == 22) {
    constexpr double ay_clock = 2000000.0;
    const double period = std::clamp(std::round(ay_clock / (16.0 * frequency)), 1.0, 4095.0);
    frequency = ay_clock / (16.0 * period);
  } else if (waveform == 26) {
    constexpr double pce_clock = 3579545.0;
    const double period = std::clamp(std::round(pce_clock / (32.0 * frequency)), 1.0, 4095.0);
    frequency = pce_clock / (32.0 * period);
  } else if (waveform == 38 || waveform == 39) {
    const double sid_clock = p->params[kClockMode].load(std::memory_order_relaxed) >= 0.5 ? 985248.0 : 1022727.0;
    const double acc = std::clamp(std::round(frequency * 16777216.0 / sid_clock), 1.0, 65535.0);
    frequency = acc * sid_clock / 16777216.0;
  } else if(waveform==44&&v.key<60){
    frequency*=std::pow(0.96745,(60-v.key)/12.0);
  }
  const double increment = std::min(0.49, frequency / p->sample_rate);
  float value = 0.0f;
  const int shape = static_cast<int>(p->params[kExpansionShape].load(std::memory_order_relaxed));
  if (waveform == 0) {
    const int duty = static_cast<int>(p->params[kDuty].load(std::memory_order_relaxed));
    value = yanes::pulse(v.phase, increment, kDuties[std::clamp(duty, 0, 3)]);
  } else if (waveform == 1) {
    // Four subsamples reduce the staircase oscillator's aliases without changing its 32 levels.
    for (int n = 0; n < 4; ++n)
      value += yanes::nes_tnd_shape(
          yanes::nes_triangle(std::fmod(v.phase + increment * n / 4.0, 1.0)));
    value *= 0.25f;
  } else if (waveform == 2) {
    const int base_period = static_cast<int>(p->params[kNoisePeriod].load(std::memory_order_relaxed));
    // One period-table entry per semitone, wrapping every sixteen.
    const int period_index =
        yanes::nes_noise_index(std::clamp(base_period, 0, 15), v.key - 60);
    const double clocks_per_sample =
        (chip_clock / yanes::kNoisePeriods[static_cast<size_t>(period_index)]) / p->sample_rate;
    v.noise_phase += clocks_per_sample;
    while (v.noise_phase >= 1.0) {
      const bool short_mode = p->params[kNoiseMode].load(std::memory_order_relaxed) >= 0.5;
      v.noise_value = v.lfsr.clock(short_mode);
      v.noise_phase -= 1.0;
    }
    value = v.noise_value;
  } else if (waveform == 3) {
    // The VRC6 pulse generator provides eight duty settings from 1/16 to 8/16.
    value = yanes::pulse(v.phase, increment, (std::clamp(shape, 0, 7) + 1) / 16.0);
  } else if (waveform == 4) {
    // Shape spans the saw's 6-bit accumulator rate, so the top of the range
    // reaches the overflowing setting the hardware is known for.
    value = yanes::vrc6_saw(v.phase, std::clamp(shape, 0, 7) * 8 + 7);
  } else if (waveform == 5) {
    // The FDS runs its DAC through an RC lowpass around 2 kHz, which is why the
    // chip sounds so much duller than the wavetable it is playing.
    const double fds_alpha =
        1.0 - std::exp(-6.28318530718 * 2000.0 / p->sample_rate);
    v.fds_lp += fds_alpha * (yanes::fds_wave(v.phase, shape) - v.fds_lp);
    value = static_cast<float>(v.fds_lp);
  } else if (waveform == 6) {
    value = yanes::n163_wave(v.phase, shape);
  } else if (waveform == 7) {
    const double ratio = p->params[kFmRatio].load(std::memory_order_relaxed);
    const double index = p->params[kFmIndex].load(std::memory_order_relaxed);
    value = yanes::vrc7_fm(v.phase, ratio, index);
  } else if (waveform == 8) {
    value = yanes::pulse(v.phase, increment, 0.5);
  } else if (waveform == 9) {
    const auto& dpcm_bank = v.dpcm_data;
    const int rate = static_cast<int>(p->params[kDpcmRate].load(std::memory_order_relaxed));
    const auto& dpcm_periods = p->params[kClockMode].load(std::memory_order_relaxed) >= 0.5
                                   ? yanes::kDpcmPeriodsPal : yanes::kDpcmPeriods;
    v.dpcm_phase += (chip_clock / dpcm_periods[std::clamp(rate, 0, 15)]) / p->sample_rate;
    while (v.dpcm_phase >= 1.0) {
      const size_t trim_start=dpcm_bank?static_cast<size_t>(static_cast<double>(dpcm_bank->size()*8U)*p->params[kDpcmTrimStart].load()):0;
      const size_t trim_end=dpcm_bank?static_cast<size_t>(static_cast<double>(dpcm_bank->size()*8U)*p->params[kDpcmTrimEnd].load()):0;
      if (dpcm_bank && !dpcm_bank->empty() && v.dpcm_bit < std::max(trim_start+1,trim_end)) {
        const bool bit = ((*dpcm_bank)[v.dpcm_bit >> 3U] >> (v.dpcm_bit & 7U)) & 1U;
        v.dpcm_level = std::clamp(v.dpcm_level + (bit ? 2 : -2), 0, 127);
        ++v.dpcm_bit;
      } else if (dpcm_bank && !dpcm_bank->empty()) {
        const uint32_t loop_mask=static_cast<uint32_t>(p->params[kDpcmLoopMask].load());
        if(loop_mask&(1U<<v.dpcm_slot))v.dpcm_bit=trim_start;else v.releasing = true;
      } else {
        const double t = v.dpcm_position;
        const bool snare = (v.key & 1) != 0;
        const double target = snare
            ? (((v.console_lfsr & 1U) ? 1.0 : -1.0) * std::exp(-t * 9.0))
            : (std::sin(6.28318530718 * (95.0 * t - 70.0 * t * t)) * std::exp(-t * 11.0));
        v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, 1, 15);
        const int desired = std::clamp(static_cast<int>(64.0 + target * 60.0), 0, 127);
        v.dpcm_level = std::clamp(v.dpcm_level + (desired >= v.dpcm_level ? 2 : -2), 0, 127);
      }
      v.dpcm_position += dpcm_periods[std::clamp(rate, 0, 15)] / chip_clock;
      v.dpcm_phase -= 1.0;
    }
    value = static_cast<float>((v.dpcm_level - 64) / 64.0);
    if ((!dpcm_bank || dpcm_bank->empty()) && v.dpcm_position > 0.65) v.releasing = true;
  } else if (waveform == 10) {
    value = yanes::pulse(v.phase, increment, kDuties[std::clamp(static_cast<int>(p->params[kDuty].load()), 0, 3)]);
  } else if (waveform == 11) {
    // Game Boy CH3: 32 four-bit samples. Shape 7 is the plain ramp the chip
    // holds after a reset, which is what the hardware reference renders play.
    const double wp = std::floor(v.phase * 32.0) / 32.0;
    value = shape == 7
                ? yanes::quantize_bipolar(2.0 * wp - 1.0, 16)
                : yanes::quantize_bipolar(std::sin(6.28318530718 * wp) +
                                              0.2 * std::sin(12.56637061436 * wp),
                                          16);
  } else if (waveform == 12) {
    const bool width7 = p->params[kNoiseMode].load(std::memory_order_relaxed) >= 0.5;
    // C-4 selects divisor 4 with shift 3, which clocks the register at 4096 Hz.
    v.noise_phase += yanes::game_boy_noise_hz(4096.0, v.key - 60) / p->sample_rate;
    while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::game_boy_lfsr_clock(v.console_lfsr, width7); v.noise_phase -= 1.0; }
    value = (v.console_lfsr & 1U) ? -1.0f : 1.0f;
  } else if (waveform == 13 || waveform == 15) {
    value = yanes::pulse(v.phase, increment, 0.5);
  } else if (waveform == 14 || waveform == 16) {
    // Tone-3 mode: the shift register advances once per tone period.
    v.noise_phase += frequency / p->sample_rate;
    const bool white = p->params[kNoiseMode].load(std::memory_order_relaxed) >= 0.5;
    while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::sega_psg_lfsr_clock(v.console_lfsr, white); v.noise_phase -= 1.0; }
    value = (v.console_lfsr & 1U) ? -1.0f : 1.0f;
  } else if (waveform == 17 || (waveform >= 27 && waveform <= 30)) {
    const size_t voice_index = static_cast<size_t>(&v - p->voices.data());
    const auto kind=fm_kind(waveform,selected_waveform);const int signature=waveform|(static_cast<int>(kind)<<8);
    if(v.hardware_signature!=signature){p->hardware_fm[voice_index].key_on(kind,frequency,fm_controls(p));v.hardware_signature=signature;}
    value = p->hardware_fm[voice_index].render(p->sample_rate, frequency);
  } else if (waveform == 22) {
    value = yanes::pulse(v.phase, increment, 0.5);
  } else if (waveform == 23) {
    v.noise_phase += frequency / p->sample_rate;
    while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, 3, 17); v.noise_phase -= 1.0; }
    value = (v.console_lfsr & 1U) ? -1.0f : 1.0f;
  } else if (waveform == 24 || waveform == 25) {
    // POKEY clocks AUDC distortion through independent 4/5/9/17-bit polynomials.
    const double pokey_clock = 1789790.0;
    const int distortion = waveform == 25 ? std::max(1, shape) : shape;
    const bool fast_clock = p->params[kNoiseMode].load(std::memory_order_relaxed) >= 0.5;
    const double divider = fast_clock ? 1.0 : 28.0;
    const bool paired = static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed)) == 34 &&
                        p->params[kStrictHardware].load(std::memory_order_relaxed) >= 0.5 && (v.channel & 1);
    const double max_audf = paired ? 65535.0 : 255.0;
    const double audf = std::clamp(std::round(pokey_clock / (divider * frequency * 2.0) - 1.0), 0.0, max_audf);
    v.noise_phase += pokey_clock / (divider * (audf + 1.0)) / p->sample_rate;
    while (v.noise_phase >= 1.0) {
      auto clock_poly = [](uint32_t state, unsigned tap, unsigned width) {
        const uint32_t feedback = ((state >> 0U) ^ (state >> tap)) & 1U;
        return ((state >> 1U) | (feedback << (width - 1U))) & ((1U << width) - 1U);
      };
      v.pokey_poly4 = clock_poly(v.pokey_poly4, 1, 4);
      v.pokey_poly5 = clock_poly(v.pokey_poly5, 2, 5);
      v.pokey_poly9 = clock_poly(v.pokey_poly9, 4, 9);
      v.pokey_poly17 = clock_poly(v.pokey_poly17, 5, 17);
      const bool tone = v.noise_value < 0.0f;
      const bool p4 = (v.pokey_poly4 & 1U) != 0, p5 = (v.pokey_poly5 & 1U) != 0;
      const bool long_poly = (fast_clock ? v.pokey_poly9 : v.pokey_poly17) & 1U;
      bool bit = tone;
      switch (distortion & 7) {
        case 0: bit = tone; break;
        case 1: bit = long_poly; break;
        case 2: bit = p4; break;
        case 3: bit = p5; break;
        case 4: bit = tone && p5; break;
        case 5: bit = p4 && p5; break;
        case 6: bit = long_poly && p5; break;
        case 7: bit = tone != long_poly; break;
      }
      v.noise_value = bit ? 1.0f : -1.0f;
      v.noise_phase -= 1.0;
    }
    value = v.noise_value;
  } else if (waveform == 26) {
    value = yanes::pce_wave(v.phase, shape);
  } else if (waveform == 37) {
    v.noise_phase += frequency / p->sample_rate;
    while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::pce_lfsr_clock(v.console_lfsr); v.noise_phase -= 1.0; }
    value = (v.console_lfsr & 1U) ? 1.0f : -1.0f;
  } else if (waveform == 38 || waveform == 39) {
    // SID's oscillator DAC combines selected 12-bit waveforms; the older 6581 exhibits
    // substantially more inter-bit bleed and lower combined-waveform amplitude than the 8580.
    const double duty = kDuties[std::clamp(static_cast<int>(p->params[kDuty].load()), 0, 3)];
    const uint16_t saw = static_cast<uint16_t>(std::floor(v.phase * 4096.0)) & 0xfffU;
    const uint16_t tri = static_cast<uint16_t>(std::floor((v.phase < 0.5 ? v.phase * 2.0 : 2.0 - v.phase * 2.0) * 4095.0));
    const uint16_t pulse = v.phase < duty ? 0xfffU : 0U;
    uint16_t dac = saw;
    switch (shape) {
      case 0: dac = saw; break; case 1: dac = tri; break; case 2: dac = pulse; break;
      case 3: dac = saw & pulse; break; case 4: dac = tri & pulse; break;
      case 5: dac = saw & tri; break; case 6: dac = saw & tri & pulse; break;
      default: v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, 5, 23); dac = static_cast<uint16_t>(v.console_lfsr & 0xfffU); break;
    }
    double raw = static_cast<double>(dac) / 2047.5 - 1.0;
    if (waveform == 38 && shape >= 3 && shape <= 6) {
      // Approximate NMOS DAC line coupling without importing measured reSID tables.
      uint16_t bled = dac;
      bled |= static_cast<uint16_t>((dac << 1U) & 0xfffU); bled |= static_cast<uint16_t>(dac >> 1U);
      raw = (static_cast<double>(bled) / 2047.5 - 1.0) * 0.42;
    }
    const double cutoff_control = p->params[kChipCutoff].load(std::memory_order_relaxed);
    const double cutoff = waveform == 38
        ? 30.0 + 12000.0 * std::pow(cutoff_control / 16000.0, 1.65)
        : 30.0 + cutoff_control * 0.94;
    const double f = std::clamp(2.0 * std::sin(3.14159265359 * cutoff / p->sample_rate), 0.0, 0.95);
    const double q = 1.6 - 1.45 * p->params[kChipResonance].load(std::memory_order_relaxed);
    v.chip_lp += f * v.chip_bp;
    const double hp = raw - v.chip_lp - q * v.chip_bp;
    v.chip_bp += f * hp;
    value = static_cast<float>(std::tanh(v.chip_lp * (waveform == 38 ? 1.8 : 1.15)));
  } else if (waveform == 40) {
    value = yanes::scc_wave(v.phase, shape);
  } else if (waveform == 42) {
    value = yanes::pulse(v.phase, increment, 0.5);
    if (p->params[kNoiseMode].load(std::memory_order_relaxed) >= 0.5 && (v.channel == 2 || v.channel == 5)) {
      v.noise_phase += frequency / p->sample_rate;
      while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, 3, 17); v.noise_phase -= 1.0; }
      value = 0.55f * value + 0.45f * ((v.console_lfsr & 1U) ? 1.0f : -1.0f);
    }
  } else if (waveform == 44) {
    // Below C-4 the TIA's 5-bit divider runs out and it has to fall back on the
    // divide-by-31 mode to reach the pitch (see the matching frequency trim
    // above). That mode is high for 18 of its 31 counts rather than square,
    // which is where the even harmonics in a low TIA note come from.
    if (shape == 0)
      value = yanes::pulse(v.phase, increment, v.key < 60 ? 18.0 / 31.0 : 0.5);
    else {
      const unsigned widths[] = {4, 5, 9, 5, 9, 4, 5, 9};
      const unsigned width = widths[std::clamp(shape, 0, 7)];
      v.noise_phase += frequency / p->sample_rate;
      while (v.noise_phase >= 1.0) { v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, width > 5 ? 4U : 1U, width); v.noise_phase -= 1.0; }
      value = (v.console_lfsr & 1U) ? 1.0f : -1.0f;
    }
  } else if (waveform == 46) {
    value = yanes::morph_wavetable(v.phase, p->params[kWavetablePosition].load(std::memory_order_relaxed),
                                   p->params[kWavetableWarp].load(std::memory_order_relaxed));
  } else if (waveform == 47) {
    value = yanes::phase_distortion(v.phase, p->params[kWavetablePosition].load(std::memory_order_relaxed),
                                    p->params[kWavetableWarp].load(std::memory_order_relaxed));
  } else if (waveform == 48) {
    value = yanes::additive(v.phase, p->params[kAdditiveTilt].load(std::memory_order_relaxed),
                            p->params[kWavetablePosition].load(std::memory_order_relaxed));
  } else if (waveform == 49) {
    value = yanes::six_operator_fm(v.phase, static_cast<int>(p->params[kGenesisAlgorithm].load(std::memory_order_relaxed)),
                                   p->params[kFmIndex].load(std::memory_order_relaxed),
                                   p->params[kFmBrightness].load(std::memory_order_relaxed));
  } else if (waveform == 50) {
    const double transient = std::exp(-static_cast<double>(v.samples) / (p->sample_rate * 0.045));
    const float digital = yanes::morph_wavetable(v.phase, p->params[kWavetablePosition].load(std::memory_order_relaxed), 0.5);
    const float partial = yanes::additive(v.phase, 0.72, 0.3);
    value = digital * 0.55f + partial * 0.35f + static_cast<float>(transient) * v.noise_value * 0.1f;
  } else if (waveform == 51) {
    value = yanes::porta_fm(v.phase,p->params[kFmRatio].load(std::memory_order_relaxed),
                            p->params[kFmIndex].load(std::memory_order_relaxed),
                            p->params[kFmBrightness].load(std::memory_order_relaxed));
  } else if (waveform == 52 || waveform == 53) {
    const double shape_norm=static_cast<double>(shape)/7.0;
    const double raw=yanes::analog_poly(v.phase,v.aux_phase,shape_norm);
    const double age=elapsed_samples/p->sample_rate;
    const double brass_sweep=waveform==53?5200.0*std::exp(-age*5.5):0.0;
    const double cutoff=std::clamp(p->params[kChipCutoff].load(std::memory_order_relaxed)+brass_sweep,40.0,p->sample_rate*0.42);
    const double f=std::clamp(2.0*std::sin(3.14159265359*cutoff/p->sample_rate),0.0,0.92);
    const double q=1.55-1.35*p->params[kChipResonance].load(std::memory_order_relaxed);
    v.chip_lp+=f*v.chip_bp;const double hp=raw-v.chip_lp-q*v.chip_bp;v.chip_bp+=f*hp;
    value=static_cast<float>(std::tanh(v.chip_lp*1.25));
    v.aux_phase=std::fmod(v.aux_phase+increment*(waveform==53?0.996:1.0045),1.0);
  } else if (waveform == 54) {
    const double transient=std::exp(-elapsed_samples/(p->sample_rate*0.075));
    value=yanes::digital_ensemble(v.phase,p->params[kWavetablePosition].load(std::memory_order_relaxed));
    value=static_cast<float>(value*0.88+v.noise_value*transient*0.12);
  } else if (waveform == 55) {
    value=yanes::tine_piano(v.phase,p->params[kFmIndex].load(std::memory_order_relaxed),
                            p->params[kFmBrightness].load(std::memory_order_relaxed),elapsed_samples/p->sample_rate);
  } else if (waveform == 56) {
    const double saw=v.phase*2.0-1.0,sub=v.aux_phase<0.5?1.0:-1.0;
    const double raw=saw*0.78+sub*0.22;
    const double cutoff=std::clamp(p->params[kChipCutoff].load(std::memory_order_relaxed),40.0,p->sample_rate*0.42);
    const double f=std::clamp(2.0*std::sin(3.14159265359*cutoff/p->sample_rate),0.0,0.9);
    const double q=1.48-1.3*p->params[kChipResonance].load(std::memory_order_relaxed);
    v.chip_lp+=f*v.chip_bp;const double hp=std::tanh(raw*1.4)-v.chip_lp-q*v.chip_bp;v.chip_bp+=f*hp;
    value=static_cast<float>(std::tanh(v.chip_lp*1.7));
    v.aux_phase=std::fmod(v.aux_phase+increment*0.5,1.0);
  } else if (waveform == 57 || waveform == 58) {
    const int drum=waveform==58?std::array<int,6>{0,1,2,4,5,6}[std::clamp(v.channel-9,0,5)]:(((v.key-36)%12+12)%12);
    const double t=elapsed_samples/p->sample_rate;
    const double pitch_ratio=frequency/std::max(1.0,yanes::midi_frequency(static_cast<double>(v.key)));
    const double character=static_cast<double>(shape)/7.0;
    auto noise=[&](double rate,unsigned tap,unsigned width){v.noise_phase+=rate*pitch_ratio/p->sample_rate;while(v.noise_phase>=1.0){v.console_lfsr=yanes::lfsr_clock(v.console_lfsr,tap,width);v.noise_phase-=1.0;}return (v.console_lfsr&1U)?1.0:-1.0;};
    constexpr double tau=6.2831853071795864769;
    double drum_value=0.0,duration=0.5;
    switch(drum){
      case 0:{const double cycles=(48.0+character*18.0)*t+(145.0+character*55.0)*(1.0-std::exp(-t*22.0))/22.0;drum_value=std::sin(tau*cycles)*std::exp(-t*(8.5-character*2.0));duration=0.7;break;}
      case 1:{const double body=std::sin(tau*185.0*pitch_ratio*t)*std::exp(-t*18.0);drum_value=(noise(10500.0,1,15)*0.78*std::exp(-t*(13.0+character*8.0))+body*0.32);duration=0.46;break;}
      case 2:{const double cycles=(105.0+character*75.0)*pitch_ratio*t+85.0*(1.0-std::exp(-t*13.0))/13.0;drum_value=std::sin(tau*cycles)*std::exp(-t*7.5);duration=0.82;break;}
      case 3:{const double p=tau*(170.0+character*130.0)*pitch_ratio*t;drum_value=std::sin(p+2.8*std::sin(p*2.73))*std::exp(-t*10.0);duration=0.62;break;}
      case 4:drum_value=noise(18500.0,1,15)*std::exp(-t*(42.0-character*9.0));duration=0.18;break;
      case 5:drum_value=noise(15800.0,1,15)*std::exp(-t*(8.0+character*3.0));duration=0.82;break;
      case 6:{double bursts=0.0;for(const double onset:{0.0,0.026,0.052,0.085})if(t>=onset)bursts+=std::exp(-(t-onset)*38.0);drum_value=noise(9200.0,3,17)*std::min(1.0,bursts);duration=0.48;break;}
      case 7:{const double cycles=95.0*pitch_ratio*t+540.0*(1.0-std::exp(-t*12.0))/12.0;const double phase=cycles-std::floor(cycles);drum_value=(phase<0.32?1.0:-1.0)*std::exp(-t*9.0);duration=0.62;break;}
      case 8:{const double poly=noise(5200.0+character*4800.0,2,5);const double tone=std::sin(tau*310.0*pitch_ratio*t);drum_value=(poly*0.58+tone*0.42)*std::exp(-t*11.0);duration=0.54;break;}
      case 9:{const double a=std::fmod(540.0*pitch_ratio*t,1.0)<0.5?1.0:-1.0;const double b=std::fmod(800.0*pitch_ratio*t,1.0)<0.5?1.0:-1.0;drum_value=(a+b)*0.5*std::exp(-t*8.0);duration=0.72;break;}
      case 10:drum_value=noise(2400.0+character*2600.0,1,5)*std::exp(-t*55.0);duration=0.14;break;
      default:{const double stepped=std::floor(std::sin(tau*126.0*pitch_ratio*t)*4.0)/4.0;drum_value=(noise(6800.0,4,9)*0.6+stepped*0.4)*std::exp(-t*12.0);duration=0.5;break;}
    }
    value=static_cast<float>(std::tanh(drum_value*1.35));
    if(t>duration)v.releasing=true;
  } else {
    const int algorithm = static_cast<int>(p->params[kGenesisAlgorithm].load(std::memory_order_relaxed));
    const double feedback = p->params[kGenesisFeedback].load(std::memory_order_relaxed);
    value = yanes::genesis_fm(v.phase, algorithm, feedback);
  }
  const int layer_mode = static_cast<int>(p->params[kLayerMode].load(std::memory_order_relaxed));
  const double layer_mix = p->params[kLayerMix].load(std::memory_order_relaxed);
  if (layer_mode > 0 && layer_mix > 0.0) {
    float layer = 0.0f;
    if (layer_mode == 1) layer = yanes::pulse(v.layer_phase, std::min(0.49, increment * 2.0), 0.5);
    else if (layer_mode == 2) layer = static_cast<float>(std::sin(6.28318530718 * v.layer_phase));
    else if (layer_mode == 3) layer = yanes::pulse(v.layer_phase, increment * 0.5, 0.5);
    else if (layer_mode == 4) layer = yanes::nes_triangle(v.layer_phase);
    else {
      v.console_lfsr = yanes::lfsr_clock(v.console_lfsr, 1, 15);
      layer = (v.console_lfsr & 1U) ? 1.0f : -1.0f;
    }
    value = static_cast<float>(value * (1.0 - layer_mix) + layer * layer_mix);
    const double ratio = layer_mode == 1 ? 2.0 : (layer_mode == 2 ? 1.5 : (layer_mode == 3 ? 0.5 : 1.0));
    v.layer_phase = std::fmod(v.layer_phase + increment * ratio, 1.0);
  }
  v.phase += increment;
  v.phase -= std::floor(v.phase);
  const bool velocity_enabled = p->params[kVelocity].load(std::memory_order_relaxed) >= 0.5;
  double level = v.env;
  if (p->params[kHardwareEnvelope].load(std::memory_order_relaxed) >= 0.5) {
    const double rate = p->params[kEnvelopeRate].load(std::memory_order_relaxed);
    const double ticks = elapsed_samples * 240.0 / p->sample_rate;
    level *= std::max(0.0, 15.0 - std::floor(ticks / std::max(1.0, 16.0 - rate))) / 15.0;
  }
  ++v.samples;
  // Brightness expression bends the voice either side of its neutral centre.
  // The centre has to be transparent: shaping every voice by default put a soft
  // saturation on the oscillator that no chip's reference render has, which cost
  // the NES triangle around 8 dB of third harmonic.
  const double brightness_bend = (v.brightness_expression - 0.5) * 2.0;
  if (std::abs(brightness_bend) > 1e-9) {
    const double drive = 0.75 + v.brightness_expression * 0.5;
    const double shaped = std::tanh(value * drive) / std::tanh(drive);
    value = static_cast<float>(value + (shaped - value) * brightness_bend);
  }
  const double expressive_level = v.volume_expression * (1.0 + v.pressure_expression * 0.35);
  return value * static_cast<float>(level * expressive_level * (velocity_enabled ? v.velocity : 1.0));
}

float process_retro(Plugin* p, float input) {
  const double amount = p->params[kRetroAmount].load(std::memory_order_relaxed);
  if (amount <= 0.00001) return input;
  const double target_rate = std::min(p->sample_rate, p->params[kOutputRate].load(std::memory_order_relaxed));
  p->hold_phase += target_rate / p->sample_rate;
  if (p->hold_phase >= 1.0) { p->held_sample = input; p->hold_phase -= 1.0; }
  const int bits = static_cast<int>(p->params[kBitDepth].load(std::memory_order_relaxed));
  const double levels = std::exp2(std::clamp(bits, 4, 16) - 1);
  double sample = std::round(p->held_sample * levels) / levels;
  // Approximate console coupling capacitor and a bandwidth-limited TV speaker.
  const double hp_a = std::exp(-6.28318530718 * 70.0 / p->sample_rate);
  p->hp_y = hp_a * (p->hp_y + sample - p->hp_x); p->hp_x = sample;
  const double cutoff = 14000.0 - 10500.0 * p->params[kSpeaker].load(std::memory_order_relaxed);
  const double lp_a = 1.0 - std::exp(-6.28318530718 * cutoff / p->sample_rate);
  p->lp_y += lp_a * (p->hp_y - p->lp_y);
  p->effect_rng = p->effect_rng * 1664525U + 1013904223U;
  const double noise = (static_cast<double>(p->effect_rng) / 2147483648.0 - 1.0) *
                       p->params[kRfNoise].load(std::memory_order_relaxed) * 0.025;
  const double mains = p->params[kClockMode].load(std::memory_order_relaxed) >= 0.5 ? 50.0 : 60.0;
  p->hum_phase = std::fmod(p->hum_phase + mains / p->sample_rate, 1.0);
  const double hum = std::sin(6.28318530718 * p->hum_phase) * p->params[kHum].load(std::memory_order_relaxed) * 0.02;
  const double colored = std::tanh((p->lp_y + noise + hum) * (1.0 + 2.5 * p->params[kSpeaker].load(std::memory_order_relaxed)));
  return static_cast<float>(input * (1.0 - amount) + colored * amount);
}

struct StereoSample { float left{}, right{}; };
StereoSample process_rack(Plugin* p, float input) {
  const float driven = std::tanh(input * static_cast<float>(1.0 + 7.0 * p->params[kDrive].load(std::memory_order_relaxed)));
  if (p->delay_buffer.empty()) return {driven, driven};
  const size_t size = p->delay_buffer.size();
  double echo_seconds = p->params[kEchoTime].load(std::memory_order_relaxed) * 0.001;
  if (p->params[kTempoSync].load(std::memory_order_relaxed) >= 0.5) {
    constexpr double beat_lengths[] = {0.0625, 0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
    const int division = static_cast<int>(p->params[kSyncDivision].load(std::memory_order_relaxed));
    echo_seconds = 60.0 / p->tempo * beat_lengths[std::clamp(division, 0, 7)];
  }
  const size_t echo_samples = std::clamp<size_t>(static_cast<size_t>(echo_seconds * p->sample_rate), 1, size - 1);
  const float delayed = p->delay_buffer[(p->delay_write + size - echo_samples) % size];
  const float feedback = static_cast<float>(p->params[kEchoFeedback].load(std::memory_order_relaxed));
  p->delay_buffer[p->delay_write] = driven + delayed * feedback;

  p->chorus_phase = std::fmod(p->chorus_phase + p->params[kChorusRate].load(std::memory_order_relaxed) / p->sample_rate, 1.0);
  const double base = 0.014 * p->sample_rate;
  const double depth = p->params[kChorusDepth].load(std::memory_order_relaxed) * 0.001 * p->sample_rate;
  auto chorus_tap = [&](double phase_offset) {
    const double modulation = (std::sin(6.28318530718 * (p->chorus_phase + phase_offset)) * 0.5 + 0.5) * depth;
    const size_t tap = std::clamp<size_t>(static_cast<size_t>(base + modulation), 1, size - 1);
    return p->delay_buffer[(p->delay_write + size - tap) % size];
  };
  const float chorus_l = chorus_tap(0.0);
  const float chorus_r = chorus_tap(0.5);
  p->delay_write = (p->delay_write + 1) % size;
  const float echo_mix = static_cast<float>(p->params[kEchoMix].load(std::memory_order_relaxed));
  const float chorus_mix = static_cast<float>(p->params[kChorusMix].load(std::memory_order_relaxed));
  const float echoed = driven * (1.0f - echo_mix) + delayed * echo_mix;
  return {echoed * (1.0f - chorus_mix) + chorus_l * chorus_mix,
          echoed * (1.0f - chorus_mix) + chorus_r * chorus_mix};
}

#ifdef YANES_HAS_EDITOR
void gui_destroy(const clap_plugin_t* plugin);
bool gui_is_open(const Plugin* p);
#endif
bool plugin_init(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  p->initialized = true;
#if defined(YANES_HAS_EDITOR) && defined(__linux__)
  // init() is the first point where asking the host for extensions is legal, and the editor
  // needs both answers before it can decide whether it can open at all.
  if (p->host && p->host->get_extension) {
    p->host_timers = static_cast<const clap_host_timer_support_t*>(
        p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT));
    p->host_fds = static_cast<const clap_host_posix_fd_support_t*>(
        p->host->get_extension(p->host, CLAP_EXT_POSIX_FD_SUPPORT));
  }
#endif
  const char* paths = std::getenv("YANES_DPCM_BANK");
  if (!paths || !*paths) return true;
  std::string_view list(paths);
  for (size_t slot = 0, at = 0; slot < p->dpcm_banks.size() && at <= list.size(); ++slot) {
#ifdef _WIN32
    constexpr char bank_separator = ';';
#else
    constexpr char bank_separator = ':';
#endif
    const size_t end = list.find(bank_separator, at);
    const std::string path(list.substr(at, end == std::string_view::npos ? list.size() - at : end - at));
    if (!path.empty()) if(auto bank=load_dpcm_file(path))install_dpcm_bank(p,slot,std::move(bank));
    if (end == std::string_view::npos) break;
    at = end + 1;
  }
  return true;
}
void plugin_destroy(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  p->initialized = false;
#ifdef YANES_HAS_EDITOR
  if (gui_is_open(p)) gui_destroy(plugin);
#endif
  delete p;
}
bool plugin_activate(const clap_plugin_t* plugin, double rate, uint32_t, uint32_t) {
  auto* p = self(plugin);
  p->sample_rate = rate;
  p->delay_buffer.assign(static_cast<size_t>(std::max(2.0, rate * 2.0)), 0.0f);
  p->delay_write = 0;
  p->output_dc_x_l=p->output_dc_y_l=p->output_dc_x_r=p->output_dc_y_r=0.0;
  p->output_peak_l.store(0.0f);p->output_peak_r.store(0.0f);p->output_clipped.store(false);
  return rate > 0.0;
}
void plugin_deactivate(const clap_plugin_t*) {}
bool plugin_start(const clap_plugin_t*) { return true; }
void plugin_stop(const clap_plugin_t*) {}
void plugin_reset(const clap_plugin_t* plugin) {
  auto* p = self(plugin);
  for (size_t i = 0; i < p->voices.size(); ++i) { p->voices[i] = Voice{}; p->hardware_fm[i].reset(); }
  p->sustain_pedal.fill(false);
  std::fill(p->delay_buffer.begin(), p->delay_buffer.end(), 0.0f);
  p->delay_write = 0; p->pitch_bend = 0; p->mod_wheel = 0;
  p->output_dc_x_l=p->output_dc_y_l=p->output_dc_x_r=p->output_dc_y_r=0.0;
  p->output_peak_l.store(0.0f);p->output_peak_r.store(0.0f);p->output_clipped.store(false);
}

clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process) {
  auto* p = self(plugin);
  if (process->audio_outputs_count == 0 || !process->audio_outputs[0].data32) return CLAP_PROCESS_ERROR;
  auto& out = process->audio_outputs[0];
  if (process->transport && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && process->transport->tempo > 1.0)
    p->tempo = process->transport->tempo;
  const uint64_t revision=p->fm_revision.load(std::memory_order_acquire);
  if(revision!=p->applied_fm_revision){const auto controls=fm_controls(p);for(auto& core:p->hardware_fm)core.update_controls(controls);p->applied_fm_revision=revision;}
  const clap_output_events_t* out_events = process->out_events;
  emit_gui_events(p, out_events);
  const uint32_t event_count = process->in_events ? process->in_events->size(process->in_events) : 0;
  uint32_t event_index = 0;
  float block_peak_l=0.0f,block_peak_r=0.0f;bool block_clipped=false;
  const double dc_r=std::exp(-2.0*3.14159265358979323846*0.05/p->sample_rate);
  auto take_events = [&](uint32_t frame, bool end_of_block) {
    while (event_index < event_count) {
      const auto* event = process->in_events->get(process->in_events, event_index);
      if (!event) break;
      if (!end_of_block && event->time > frame) break;
      handle_event(p, event, out_events, event->time);
      ++event_index;
    }
  };
  for (uint32_t frame = 0; frame < process->frames_count; ++frame) {
    take_events(frame, false);
    float sample = 0.0f;
    const bool nes_stack = static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed)) == 18;
    double pulse_dac = 0.0, triangle_dac = 0.0, noise_dac = 0.0, dpcm_dac = 0.0;
    const uint32_t mute_mask=static_cast<uint32_t>(p->params[kStackMuteMask].load());
    const uint32_t solo_mask=static_cast<uint32_t>(p->params[kStackSoloMask].load());
    for (auto& v : p->voices) if (v.active) {
      const uint32_t channel_bit=1U<<std::clamp<int>(v.channel,0,15);
      const bool muted=(mute_mask&channel_bit)||(solo_mask&&!(solo_mask&channel_bit));
      const float rendered = render_voice(p, v);
      if (!v.active) emit_note_end(out_events, frame, v);
      if (muted) continue;
      if (!nes_stack) { sample += rendered; continue; }
      const double velocity = p->params[kVelocity].load(std::memory_order_relaxed) >= 0.5 ? v.velocity : 1.0;
      const double amplitude = std::max(0.000001, v.env * velocity);
      const double bipolar = std::clamp(static_cast<double>(rendered) / amplitude, -1.0, 1.0);
      if (v.channel <= 1) pulse_dac += (bipolar * 0.5 + 0.5) * 15.0 * amplitude;
      else if (v.channel == 2) triangle_dac += (bipolar * 0.5 + 0.5) * 15.0 * amplitude;
      else if (v.channel == 3) noise_dac += (bipolar * 0.5 + 0.5) * 15.0 * amplitude;
      else if (v.channel == 4) dpcm_dac += (bipolar * 0.5 + 0.5) * 127.0 * amplitude;
    }
    if (nes_stack && (pulse_dac > 0.0 || triangle_dac > 0.0 || noise_dac > 0.0 || dpcm_dac > 0.0)) {
      const double pulse_out = pulse_dac > 0.0 ? 95.88 / (8128.0 / pulse_dac + 100.0) : 0.0;
      const double tnd_input = triangle_dac / 8227.0 + noise_dac / 12241.0 + dpcm_dac / 22638.0;
      const double tnd_out = tnd_input > 0.0 ? 159.79 / (1.0 / tnd_input + 100.0) : 0.0;
      sample = static_cast<float>((pulse_out + tnd_out) * 2.0 - 0.65);
    }
    const double voice_gain = db_gain(p->params[kGainDb].load(std::memory_order_relaxed));
    const double master = db_gain(p->params[kMasterDb].load(std::memory_order_relaxed));
    sample = std::tanh(sample * static_cast<float>(voice_gain)) * static_cast<float>(master);
    sample = process_retro(p, sample);
    const StereoSample effected = process_rack(p, sample);
    const float width = static_cast<float>(p->params[kStereoWidth].load(std::memory_order_relaxed));
    const double raw_l=effected.left*(1.0f+width*0.08f),raw_r=effected.right*(1.0f-width*0.08f);
    const int output_waveform=static_cast<int>(p->params[kWaveform].load(std::memory_order_relaxed));
    const bool dc_enabled=nes_stack||output_waveform==38||output_waveform==39||p->params[kRetroAmount].load(std::memory_order_relaxed)>0.00001;
    double dc_l=raw_l,dc_right=raw_r;
    if(dc_enabled){dc_l=raw_l-p->output_dc_x_l+dc_r*p->output_dc_y_l;dc_right=raw_r-p->output_dc_x_r+dc_r*p->output_dc_y_r;}
    else p->output_dc_x_l=p->output_dc_y_l=p->output_dc_x_r=p->output_dc_y_r=0.0;
    if(raw_l==0.0&&raw_r==0.0){dc_l=dc_right=0.0;p->output_dc_x_l=p->output_dc_y_l=p->output_dc_x_r=p->output_dc_y_r=0.0;}
    if(std::abs(dc_l)<1.0e-7)dc_l=0.0;
    if(std::abs(dc_right)<1.0e-7)dc_right=0.0;
    p->output_dc_x_l=raw_l;p->output_dc_y_l=dc_l;p->output_dc_x_r=raw_r;p->output_dc_y_r=dc_right;
    block_clipped=block_clipped||std::abs(dc_l)>0.995||std::abs(dc_right)>0.995;
    const float safe_l=static_cast<float>(std::clamp(dc_l,-0.995,0.995));
    const float safe_r=static_cast<float>(std::clamp(dc_right,-0.995,0.995));
    block_peak_l=std::max(block_peak_l,std::abs(safe_l));block_peak_r=std::max(block_peak_r,std::abs(safe_r));
    out.data32[0][frame]=safe_l;
    if (out.channel_count > 1) out.data32[1][frame]=safe_r;
    if((p->scope_decimator++&7U)==0U){const uint32_t at=p->scope_write.fetch_add(1,std::memory_order_relaxed);p->scope_samples[at&255U].store((effected.left+effected.right)*0.5f,std::memory_order_relaxed);p->scope_revision.fetch_add(1,std::memory_order_release);}
  }
  take_events(process->frames_count, true);
  p->output_peak_l.store(block_peak_l,std::memory_order_relaxed);p->output_peak_r.store(block_peak_r,std::memory_order_relaxed);
  p->output_clipped.store(block_clipped,std::memory_order_relaxed);
  return CLAP_PROCESS_CONTINUE;
}

uint32_t audio_count(const clap_plugin_t*, bool input) { return input ? 0U : 1U; }
bool audio_get(const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) {
  if (input || index != 0 || !info) return false;
  *info = {};
  info->id = 0;
  std::snprintf(info->name, sizeof(info->name), "Stereo output");
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}
const clap_plugin_audio_ports_t kAudioPorts{audio_count, audio_get};

uint32_t note_count(const clap_plugin_t*, bool input) { return input ? 1U : 0U; }
bool note_get(const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* info) {
  if (!input || index != 0 || !info) return false;
  *info = {};
  info->id = 0;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::snprintf(info->name, sizeof(info->name), "Notes");
  return true;
}
const clap_plugin_note_ports_t kNotePorts{note_count, note_get};

uint32_t params_count(const clap_plugin_t*) { return kParamCount; }
bool params_info(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index >= kParamCount || !info) return false;
  const auto& s = kSpecs[index];
  *info = {};
  info->id = index;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE | (s.stepped ? CLAP_PARAM_IS_STEPPED : 0);
  std::snprintf(info->name, sizeof(info->name), "%s", s.name);
  std::snprintf(info->module, sizeof(info->module), "%s", s.module);
  info->min_value = s.min; info->max_value = s.max; info->default_value = s.def;
  return true;
}
bool params_value(const clap_plugin_t* plugin, clap_id id, double* value) {
  if (id >= kParamCount || !value) return false;
  *value = self(plugin)->params[id].load(std::memory_order_relaxed);
  return true;
}
bool value_to_text(const clap_plugin_t*, clap_id id, double value, char* text, uint32_t capacity) {
  if (id >= kParamCount || !text || capacity == 0) return false;
  if (id == kWaveform) std::snprintf(text, capacity, "%s", kWaveNames[std::clamp(static_cast<int>(std::round(value)), 0, 57)]);
  else if (id == kDuty) std::snprintf(text, capacity, "%s", kDutyNames[std::clamp(static_cast<int>(std::round(value)), 0, 3)]);
  else if (id == kNoiseMode || id == kVelocity || id == kHardwareEnvelope || id == kTempoSync || id == kStrictHardware) std::snprintf(text, capacity, "%s", value >= 0.5 ? "On" : "Off");
  else if (id == kClockMode) std::snprintf(text, capacity, "%s", value >= 0.5 ? "PAL / 50 Hz" : "NTSC / 60 Hz");
  else if (id == kArpMode) std::snprintf(text, capacity, "%s", kArpNames[std::clamp(static_cast<int>(std::round(value)), 0, 5)]);
  else if (id == kLayerMode) std::snprintf(text, capacity, "%s", kLayerNames[std::clamp(static_cast<int>(std::round(value)), 0, 5)]);
  else if (id == kPreset) std::snprintf(text, capacity, "%s", kPresetNames[std::clamp(static_cast<int>(std::round(value)), 0, 48)]);
  else if (id == kAttackMs || id == kReleaseMs || id == kPortamentoMs || id == kEchoTime || id == kChorusDepth) std::snprintf(text, capacity, "%.1f ms", value);
  else if (id == kGainDb || id == kMasterDb) std::snprintf(text, capacity, "%.1f dB", value);
  else if (id == kFineTune) std::snprintf(text, capacity, "%.1f cents", value);
  else if (id == kFmRatio) std::snprintf(text, capacity, "%.2f : 1", value);
  else if (id == kFmIndex || id == kGenesisFeedback || id == kRetroAmount || id == kRfNoise || id == kHum || id == kSpeaker || id == kStereoWidth || id == kChipResonance || id == kWavetablePosition || id == kWavetableWarp || id == kAdditiveTilt || id == kFmBrightness || id == kLayerMix || id == kDrive || id == kEchoMix || id == kEchoFeedback || id == kChorusMix) std::snprintf(text, capacity, "%.2f", value);
  else if (id == kVibratoRate || id == kChorusRate) std::snprintf(text, capacity, "%.2f Hz", value);
  else if (id == kVibratoDepth) std::snprintf(text, capacity, "%.2f semitones", value);
  else if (id == kChipCutoff) std::snprintf(text, capacity, "%.0f Hz", value);
  else if (id == kOutputRate) std::snprintf(text, capacity, "%.0f Hz", value);
  else std::snprintf(text, capacity, "%.0f", value);
  return true;
}
bool text_to_value(const clap_plugin_t*, clap_id id, const char* text, double* value) {
  if (id >= kParamCount || !text || !value) return false;
  char* end = nullptr;
  const double parsed = std::strtod(text, &end);
  if (end == text || !std::isfinite(parsed)) return false;
  *value = std::clamp(parsed, kSpecs[id].min, kSpecs[id].max);
  return true;
}
void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t* out) {
  auto* p = self(plugin);
  if (in) for (uint32_t i = 0; i < in->size(in); ++i) handle_event(p, in->get(in, i), out, 0);
  emit_gui_events(p, out);
}
const clap_plugin_params_t kParams{params_count, params_info, params_value, value_to_text, text_to_value, params_flush};

struct StateBlob { uint32_t magic; uint32_t version; double values[kParamCount]; uint32_t dpcm_sizes[16]; };
bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
  if (!stream || !stream->write) return false;
  StateBlob state{0x53454e59U, 13, {}, {}};
  for (clap_id i = 0; i < kParamCount; ++i) state.values[i] = self(plugin)->params[i].load(std::memory_order_relaxed);
  std::array<std::shared_ptr<const std::vector<uint8_t>>,16> banks{};
  for (size_t i = 0; i < 16; ++i) {banks[i]=self(plugin)->dpcm_banks[i].load();state.dpcm_sizes[i]=static_cast<uint32_t>(banks[i]?std::min<size_t>(banks[i]->size(),1024U*1024U):0);}
  const auto* data = reinterpret_cast<const uint8_t*>(&state);
  uint64_t done = 0;
  while (done < sizeof(state)) { const int64_t n = stream->write(stream, data + done, sizeof(state) - done); if (n <= 0) return false; done += static_cast<uint64_t>(n); }
  for (size_t i = 0; i < 16; ++i) { done = 0;
    while (done < state.dpcm_sizes[i]) { const int64_t n = stream->write(stream, banks[i]->data() + done, state.dpcm_sizes[i] - done); if (n <= 0) return false; done += static_cast<uint64_t>(n); } }
  return true;
}
bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
  if (!stream || !stream->read) return false;
  struct Header { uint32_t magic, version; } header{};
  auto read_exact = [stream](void* destination, uint64_t size) {
    auto* bytes = static_cast<uint8_t*>(destination); uint64_t done = 0;
    while (done < size) { const int64_t n = stream->read(stream, bytes + done, size - done); if (n <= 0) return false; done += static_cast<uint64_t>(n); }
    return true;
  };
  if (!read_exact(&header, sizeof(header)) || header.magic != 0x53454e59U) return false;
  std::array<double, kParamCount> values{};
  for (clap_id i = 0; i < kParamCount; ++i) values[i] = kSpecs[i].def;
  std::array<uint32_t, 16> sizes{};
  if (header.version == 13) {
    StateBlob state{}; state.magic = header.magic; state.version = header.version;
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin());
    std::copy(std::begin(state.dpcm_sizes), std::end(state.dpcm_sizes), sizes.begin());
  } else if (header.version == 12) {
    struct Legacy12 { uint32_t magic, version; double values[77]; uint32_t dpcm_sizes[16]; } state{};
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin());std::copy(std::begin(state.dpcm_sizes),std::end(state.dpcm_sizes),sizes.begin());
  } else if (header.version == 11) {
    struct Legacy11 { uint32_t magic, version; double values[75]; uint32_t dpcm_sizes[16]; } state{};
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin());std::copy(std::begin(state.dpcm_sizes),std::end(state.dpcm_sizes),sizes.begin());
  } else if (header.version == 10) {
    struct Legacy10 { uint32_t magic, version; double values[73]; uint32_t dpcm_sizes[16]; } state{};
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin());std::copy(std::begin(state.dpcm_sizes),std::end(state.dpcm_sizes),sizes.begin());
  } else if (header.version == 9) {
    struct Legacy9 { uint32_t magic, version; double values[72]; uint32_t dpcm_size; } state{};
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin()); sizes[0] = state.dpcm_size;
  } else if (header.version == 8) {
    struct Legacy8 { uint32_t magic, version; double values[62]; uint32_t dpcm_size; } state{};
    if (!read_exact(reinterpret_cast<uint8_t*>(&state) + sizeof(header), sizeof(state) - sizeof(header))) return false;
    std::copy(std::begin(state.values), std::end(state.values), values.begin()); sizes[0] = state.dpcm_size;
  } else return false;
  for (uint32_t size : sizes) if (size > 1024U * 1024U) return false;
  // State is an exact parameter snapshot. Restoring the Preset selector must not
  // execute its recipe and overwrite the other values in that snapshot.
  for (clap_id i = 0; i < kParamCount; ++i) set_param(self(plugin), i, values[i], false);
  auto* p = self(plugin);
  for (size_t i = 0; i < 16; ++i) { auto bank=std::make_shared<std::vector<uint8_t>>(sizes[i]);if (!read_exact(bank->data(), sizes[i])) return false;install_dpcm_bank(p,i,std::move(bank)); }
  return true;
}
const clap_plugin_state_t kState{state_save, state_load};

#ifdef YANES_HAS_EDITOR
#include "ui_frontend.hpp"
#include "ui_backends.hpp"
#endif

bool voice_info_get(const clap_plugin_t*, clap_voice_info_t* info) {
  if (!info) return false;
  info->voice_count = 16;
  info->voice_capacity = 16;
  info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
  return true;
}
const clap_plugin_voice_info_t kVoiceInfo{voice_info_get};

const void* get_extension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_VOICE_INFO)) return &kVoiceInfo;
#ifdef YANES_HAS_EDITOR
  if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
#if defined(__linux__)
  if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
  if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kPosixFdSupport;
#endif
#endif
  return nullptr;
}
void on_main_thread(const clap_plugin_t* plugin) {
  // Applying a preset rewrites most parameters; the host only learns about that here, on the
  // thread where rescan is legal to call.
  auto* p = self(plugin);
  if (!p->initialized || !p->params_rescan_pending.exchange(false, std::memory_order_acq_rel) || !p->host) return;
  if (!p->host->get_extension) return;
  if (const auto* hp = static_cast<const clap_host_params_t*>(
          p->host->get_extension(p->host, CLAP_EXT_PARAMS))) {
    if (hp->rescan) hp->rescan(p->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
  }
}

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                           CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const clap_plugin_descriptor_t kDescriptor{
    CLAP_VERSION_INIT, "org.yanes.native", "YANES", "YANES contributors",
    "", "", "", "0.2.0", "Native multi-console chiptune synthesizer", kFeatures};

const clap_plugin_t* create_plugin(const clap_host_t* host) {
  auto* p = new (std::nothrow) Plugin;
  if (!p) return nullptr;
  p->host = host;
  for (clap_id i = 0; i < kParamCount; ++i) p->params[i].store(kSpecs[i].def);
  // A fresh instance is already sitting on a chip voice, so it needs that voice's
  // register state too. Without this the default voice is the one voice that never
  // gets its own defaults, because selecting it is not a change.
  apply_voice_defaults(p, static_cast<int>(kSpecs[kWaveform].def));
  p->api = {&kDescriptor, p, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
            plugin_start, plugin_stop, plugin_reset, plugin_process, get_extension, on_main_thread};
  return &p->api;
}

uint32_t factory_count(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, uint32_t index) { return index == 0 ? &kDescriptor : nullptr; }
const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!host || !id || !clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDescriptor.id)) return nullptr;
  return create_plugin(host);
}
const clap_plugin_factory_t kFactory{factory_count, factory_descriptor, factory_create};

bool entry_init(const char*) {
#ifdef __linux__
  XInitThreads();
#endif
  return true;
}
void entry_deinit() {}
const void* entry_factory(const char* id) { return id && !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

#if defined(YANES_HAS_EDITOR) && defined(__APPLE__)
@implementation YANES_EDITOR_VIEW
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)drawRect:(NSRect)dirty { (void)dirty; editor_cocoa_draw(self.plugin); }
- (void)onRefresh:(NSTimer*)timer { (void)timer; editor_cocoa_refresh(self.plugin); }
- (void)handleEvent:(NSEvent*)event action:(GuiPointer)action button:(int)button {
  const NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
  auto* p = static_cast<Plugin*>(self.plugin);
  const int x = yanes::ui::unscale_x(static_cast<int>(point.x), static_cast<int>(p->gui_width));
  const int y = yanes::ui::unscale_y(static_cast<int>(point.y), static_cast<int>(p->gui_height));
  editor_cocoa_input(self.plugin, action, button, x, y);
}
- (void)mouseDown:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Down button:1]; }
- (void)mouseDragged:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Move button:1]; }
- (void)mouseUp:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Up button:1]; }
- (void)rightMouseDown:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Down button:3]; }
- (void)rightMouseUp:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Up button:3]; }
- (void)otherMouseDown:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Down button:2]; }
- (void)otherMouseUp:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Up button:2]; }
- (void)scrollWheel:(NSEvent*)event {
  const int button = [event deltaY] > 0 ? 4 : 5;
  [self handleEvent:event action:GuiPointer::Down button:button];
}
- (void)mouseMoved:(NSEvent*)event { [self handleEvent:event action:GuiPointer::Move button:0]; }
- (void)mouseExited:(NSEvent*)event { (void)event; editor_cocoa_input(self.plugin, GuiPointer::Leave, 0, 0, 0); }
@end
#endif

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
