// The plug-in's side of the editor: an EditorHost that reads and writes Plugin parameters, the
// help text and relevance rules, and the per-page artwork (scope, mixer, sample bank, ...).
// Included inside the YANES anonymous namespace so it can see Plugin and the DSP helpers. Layout,
// widgets, and input handling live in ui_editor.hpp.
#pragma once

#include "ui_canvas.hpp"
#include "ui_editor.hpp"
#include "ui_layout.hpp"
#include "ui_pages.hpp"

bool hardware_fm_waveform(int waveform) {
  return waveform==17||(waveform>=27&&waveform<=30)||waveform==21||waveform==31||
         waveform==32||waveform==33||waveform==36;
}
bool dpcm_waveform(int waveform) { return waveform==9||waveform==18||waveform==32||waveform==33; }
bool duty_waveform(int waveform) {
  return waveform==0||waveform==3||waveform==10||waveform==18||waveform==19||waveform==38||waveform==39;
}

// Whether a control does anything for the current sound source and settings. Irrelevant controls
// stay where they are but are dimmed, so the layout never jumps when the source changes.
bool gui_param_relevant(const Plugin* p, clap_id id) {
  const auto value = [p](clap_id param) { return p->params[param].load(std::memory_order_relaxed); };
  const int waveform = static_cast<int>(value(kWaveform));
  if (id >= kSequence1 && id <= kSequence8) return static_cast<int>(value(kArpMode)) == 5;
  if (id >= kDutyStep1 && id <= kDutyStep8) return duty_waveform(waveform) && value(kDutySeqMode) >= 0.5;
  if (id >= kFmAttack && id <= kFmPmDepth) return hardware_fm_waveform(waveform);
  if (id >= kDpcmBaseKey && id <= kDpcmTrimEnd) return dpcm_waveform(waveform);
  switch(id){
    case kDuty:case kDutySeqMode:return duty_waveform(waveform);
    case kDutySeqLength:case kDutySeqRate:return duty_waveform(waveform) && value(kDutySeqMode) >= 0.5;
    case kSequenceLength:return static_cast<int>(value(kArpMode)) == 5;
    case kArpRate:return static_cast<int>(value(kArpMode)) != 0;
    case kSyncDivision:return value(kTempoSync) >= 0.5;
    case kSweepTime:return value(kSweepDepth) != 0.0;
    case kEnvelopeRate:return value(kHardwareEnvelope) >= 0.5;
    case kLayerMix:return static_cast<int>(value(kLayerMode)) != 0;
    case kNoisePeriod:return waveform==2||waveform==18;
    case kNoiseMode:return waveform==2||waveform==12||waveform==14||waveform==16||waveform==18||waveform==19||
                           waveform==20||waveform==21||waveform==23||waveform==24||waveform==25||waveform==34||waveform==42;
    case kExpansionShape:return waveform==3||waveform==4||waveform==5||waveform==6||waveform==24||waveform==25||
                                waveform==26||waveform==34||waveform==35||waveform==38||waveform==39||waveform==40||
                                waveform==41||waveform==44||waveform==45||waveform==57;
    case kFmRatio:case kFmIndex:return waveform==7||waveform==49||waveform==51||waveform==55;
    case kDpcmRate:return dpcm_waveform(waveform);
    case kGenesisAlgorithm:case kGenesisFeedback:return waveform==49||hardware_fm_waveform(waveform);
    case kChipCutoff:case kChipResonance:return waveform==38||waveform==39||waveform==52||waveform==53||waveform==56;
    case kWavetablePosition:return waveform==46||waveform==47||waveform==48||waveform==50||waveform==54;
    case kWavetableWarp:return waveform==46||waveform==47;
    case kAdditiveTilt:return waveform==48;
    case kFmBrightness:return waveform==7||waveform==49||waveform==51||waveform==55||hardware_fm_waveform(waveform);
    default:return true;
  }
}
const char* gui_param_name(clap_id id,int waveform){
  if(id!=kExpansionShape)return kSpecs[static_cast<size_t>(id)].name;
  if(waveform==57)return "Drum character";
  if(waveform==46||waveform==47||waveform==48||waveform==50||waveform==54)return "Table shape";
  if(waveform==7||waveform==49||waveform==51||waveform==55)return "FM character";
  return "Chip shape";
}
const char* gui_help(clap_id id) {
  switch(id) {
    case kWaveform:return "Selects the chip, oscillator, or MIDI-channel stack used to make sound.";
    case kDuty:return "Changes pulse width; narrower duties sound thinner and brighter.";
    case kNoisePeriod:return "Selects a hardware noise-clock period instead of a continuously tuned pitch.";
    case kNoiseMode:return "Switches the selected chip's alternate short, narrow, or white-noise behavior.";
    case kAttackMs:return "Sets how quickly a new note reaches full level.";
    case kReleaseMs:return "Sets how long a note fades after release.";
    case kExpansionShape:return "Changes the selected chip model's duty, wavetable, or distortion variant.";
    case kFmRatio:return "Sets the modulator frequency relative to the played note.";
    case kFmIndex:return "Controls FM modulation strength and harmonic complexity.";
    case kClockMode:return "Switches between NTSC and PAL timing, changing authentic pitch quantization.";
    case kRetroAmount:return "Blends in the console/television degradation section.";
    case kBitDepth:return "Reduces amplitude resolution for stepped digital grit.";
    case kOutputRate:return "Reduces effective sample rate for brighter or rougher aliasing.";
    case kChipCutoff:return "Sets the cutoff of chip-specific filtering, especially SID modes.";
    case kChipResonance:return "Emphasizes frequencies around the chip filter cutoff.";
    case kWavetablePosition:return "Morphs across sine, triangle, saw, and pulse regions.";
    case kWavetableWarp:return "Bends wavetable phase to reshape the harmonic balance.";
    case kFmBrightness:return "Changes carrier level and the perceived brightness of FM voices.";
    case kLayerMode:return "Adds a tuned or noise-based companion oscillator to every voice.";
    case kLayerMix:return "Balances the added layer against the primary oscillator.";
    case kTempoSync:return "Locks the arpeggio, duty steps, and echo timing to host tempo.";
    case kSyncDivision:return "Sets how many sequence steps play per beat while tempo sync is on.";
    case kStrictHardware:return "Hardware-like stack retriggering, and raw (non-bandlimited) NES pulses into the mixer.";
    case kArpMode:return "Chooses a built-in arpeggio, or User steps to play the pitch lane below.";
    case kArpRate:return "Sets how many arpeggio or pitch steps play per second.";
    case kSequenceLength:return "Sets how many user pitch steps play before the sequence repeats.";
    case kDutySeqMode:return "Steps through the duty lane on every note: looping, or once and then holding the last step.";
    case kDutySeqLength:return "Sets how many duty steps play; drag the lane's ruler to change it too.";
    case kDutySeqRate:return "Sets how many duty steps play per second; tempo sync uses the sync division instead.";
    case kDpcmBaseKey:return "Maps this MIDI note to sample slot 1; following notes select following slots.";
    case kDpcmLoopMask:return "Stores which of the sixteen DPCM slots repeat after reaching trim end.";
    case kDpcmInitialLevel:return "Sets the NES seven-bit DAC level before the first DPCM bit is decoded.";
    case kDpcmTrimStart:return "Moves the shared non-destructive start boundary for DPCM slots.";
    case kDpcmTrimEnd:return "Moves the shared non-destructive end boundary for DPCM slots.";
    case kStackMuteMask:return "Stores muted MIDI channels; use the channel tiles above for easier editing.";
    case kStackSoloMask:return "Stores soloed MIDI channels; use the channel tiles above for easier editing.";
    case kPreset:return "Loads a complete starting recipe; subsequent edits remain fully automatable.";
    case kPitchBendRange:return "Sets how many semitones a full pitch-wheel movement bends, up to four octaves.";
    default:break;
  }
  if(id>=kSequence1&&id<=kSequence8)return "Sets this step's pitch offset in semitones; plays when Arpeggio is User steps.";
  if(id>=kDutyStep1&&id<=kDutyStep8)return "Sets this step's pulse duty; plays when the duty sequence is on.";
  if(id>=kFmAttack&&id<=kFmRelease)return "Shapes the hardware FM operators' amplitude envelope.";
  if(id>=kFmDetune&&id<=kFmPmDepth)return "Programs the corresponding hardware FM operator or LFO register.";
  if(id>=kDrive&&id<=kChorusDepth)return "Shapes the internal drive, echo, and chorus effects rack.";
  return "Adjusts this part of the current sound; changes are immediately audible and automatable.";
}

void gui_request_flush(Plugin* p) {
  if (p->host) if (const auto* hp = static_cast<const clap_host_params_t*>(
          p->host->get_extension(p->host, CLAP_EXT_PARAMS)))
    hp->request_flush(p->host);
}

bool gui_choose_sample(Plugin* p, int slot);

void gui_mark_state(Plugin* p) {
  if (p->host) if (const auto* hs = static_cast<const clap_host_state_t*>(
          p->host->get_extension(p->host, CLAP_EXT_STATE)))
    hs->mark_dirty(p->host);
}

// Tile positions inside the hardware page's strip.
namespace strip {
constexpr int tile_y = 34, tile_h = 44;
inline yanes::ui::Rect mixer_tile(const yanes::ui::Rect& area, int channel) {
  return {area.x + 20 + channel * 46, area.y + tile_y, 40, tile_h};
}
inline yanes::ui::Rect bank_tile(const yanes::ui::Rect& area, int slot) {
  return {area.x + 800 + slot * 44, area.y + tile_y, 38, tile_h};
}
}  // namespace strip

class PluginEditorHost final : public yanes::ui::EditorHost {
 public:
  explicit PluginEditorHost(Plugin* p) : p_(p) {}

  int param_count() const override { return static_cast<int>(kParamCount); }
  yanes::ui::ParamInfo info(int id) const override {
    const auto& s = kSpecs[static_cast<size_t>(id)];
    return {s.name, s.min, s.max, s.def, s.stepped};
  }
  double value(int id) const override { return p_->params[static_cast<size_t>(id)].load(std::memory_order_relaxed); }
  void begin_edit(int id) override {
    gui_push(p_, Plugin::kGuiBegin, static_cast<clap_id>(id), 0);
    gui_request_flush(p_);
  }
  void edit(int id, double v) override {
    set_param(p_, static_cast<clap_id>(id), v);
    gui_push(p_, Plugin::kGuiValue, static_cast<clap_id>(id), value(id));
    gui_request_flush(p_);
  }
  void end_edit(int id) override {
    gui_push(p_, Plugin::kGuiEnd, static_cast<clap_id>(id), 0);
    gui_request_flush(p_);
  }
  void format(int id, double v, char* out, size_t capacity) const override {
    format_value(static_cast<uint32_t>(id), v, out, static_cast<uint32_t>(capacity));
  }
  const char* label(int id) const override {
    return gui_param_name(static_cast<clap_id>(id), static_cast<int>(value(kWaveform)));
  }
  const char* help(int id) const override { return gui_help(static_cast<clap_id>(id)); }
  const char* short_label(int id) const override {
    if (const char* name = yanes::ui::yanes_short_name(id)) return name;
    return label(id);
  }
  bool relevant(int id) const override { return gui_param_relevant(p_, static_cast<clap_id>(id)); }

  const char* strip_help(int page) const override {
    if (page == 4) return "Channel tiles: left-click mute, right-click solo  •  Sample slots: left-click loop, middle-click load, right-click clear";
    return nullptr;
  }

  void draw_meters(yanes::ui::Painter& g, const yanes::ui::Rect& r) override {
    using namespace yanes::ui::theme;
    const float peak_l = p_->output_peak_l.load(std::memory_order_relaxed);
    const float peak_r = p_->output_peak_r.load(std::memory_order_relaxed);
    const bool clipped = p_->output_clipped.load(std::memory_order_relaxed);
    g.text(r.x, r.y + 24, "OUT", muted, 50, yanes::ui::TextSize::Small);
    const int bar_x = r.x + 52, bar_w = 230;
    g.rect(bar_x, r.y + 6, bar_w, 9, border);
    g.rect(bar_x, r.y + 20, bar_w, 9, border);
    const uint32_t level = clipped ? red : green;
    g.rect(bar_x, r.y + 6, static_cast<int>(bar_w * std::clamp(peak_l, 0.0f, 1.0f)), 9, level);
    g.rect(bar_x, r.y + 20, static_cast<int>(bar_w * std::clamp(peak_r, 0.0f, 1.0f)), 9, level);
    g.text(bar_x + bar_w + 18, r.y + 24, clipped ? "CLIP" : "16 VOICES • CLAP", clipped ? red : muted, r.right() - bar_x - bar_w - 18,
           yanes::ui::TextSize::Small);
  }

  void draw_strip(int page, yanes::ui::Painter& g, const yanes::ui::Rect& r) override {
    using namespace yanes::ui::theme;
    using yanes::ui::Point;
    using yanes::ui::TextSize;
    if (page == 0) {
      std::array<float, 256> snapshot{};
      const uint32_t write = p_->scope_write.load(std::memory_order_acquire);
      for (size_t i = 0; i < snapshot.size(); ++i)
        snapshot[i] = p_->scope_samples[(write + static_cast<uint32_t>(i)) & 255U].load(std::memory_order_relaxed);
      const int scope_x = r.x + 20, scope_w = 780, mid = r.y + r.h / 2 + 8;
      g.text(scope_x, r.y + 24, "OUTPUT SCOPE", muted, 200, TextSize::Small);
      g.line(scope_x, mid, scope_x + scope_w, mid, border);
      std::vector<Point> scope;
      for (int i = 0; i < 128; ++i) {
        const float sample = (snapshot[static_cast<size_t>(i * 2)] + snapshot[static_cast<size_t>(i * 2 + 1)]) * 0.5f;
        scope.push_back({scope_x + i * scope_w / 127, mid - static_cast<int>(std::clamp(sample, -1.0f, 1.0f) * 36.0f)});
      }
      g.polyline(scope, cyan, 2);
      const int spectrum_x = r.x + 850, base = r.bottom() - 14;
      g.text(spectrum_x, r.y + 24, "SPECTRUM", muted, 180, TextSize::Small);
      constexpr double tau = 6.2831853071795864769;
      for (int band = 0; band < 32; ++band) {
        const int bin = std::clamp(static_cast<int>(std::lround(std::pow(2.0, band / 6.8))), 1, 112);
        double real = 0.0, imag = 0.0;
        for (int n = 0; n < 256; ++n) {
          const double window = 0.5 - 0.5 * std::cos(tau * n / 255.0), angle = tau * bin * n / 256.0;
          real += snapshot[static_cast<size_t>(n)] * window * std::cos(angle);
          imag -= snapshot[static_cast<size_t>(n)] * window * std::sin(angle);
        }
        const double magnitude = std::sqrt(real * real + imag * imag) / 64.0;
        const int h = std::clamp(static_cast<int>(std::log1p(magnitude * 7.0) * 26.0), 2, 60);
        g.rect(spectrum_x + band * 20, base - h, 14, h, band < 22 ? green : amber);
      }
      // Envelope sketch: attack ramp, sustain, release.
      const double attack = value(kAttackMs), release = value(kReleaseMs);
      const int ex = r.x + 1520 - 170, top = r.y + 40, bottom = r.bottom() - 16;
      g.text(ex, r.y + 24, "ENVELOPE", muted, 150, TextSize::Small);
      const int ax = ex + static_cast<int>(std::clamp(attack / 500.0, 0.0, 1.0) * 50.0);
      const int sx = ex + 100, rx = sx + static_cast<int>(std::clamp(release / 2000.0, 0.0, 1.0) * 60.0) + 4;
      g.polyline({{ex, bottom}, {ax, top}, {sx, top}, {rx, bottom}}, green, 2);
    } else if (page == 2) {
      const int wx = r.x + 20, ww = 900, mid = r.y + r.h / 2 + 10;
      g.text(wx, r.y + 24, "WAVETABLE", muted, 200, TextSize::Small);
      g.line(wx, mid, wx + ww, mid, border);
      std::vector<Point> points;
      for (int i = 0; i < 160; ++i) {
        const double phase = i / 159.0;
        points.push_back({wx + i * ww / 159,
                          mid - static_cast<int>(yanes::morph_wavetable(phase, value(kWavetablePosition), value(kWavetableWarp)) * 34)});
      }
      g.polyline(points, green, 2);
      const int fx = r.x + 980, fw = 520, base = r.bottom() - 14;
      g.text(fx, r.y + 24, "FILTER RESPONSE", muted, 240, TextSize::Small);
      const double cutoff = value(kChipCutoff), res = value(kChipResonance);
      std::vector<Point> response;
      for (int i = 0; i < 120; ++i) {
        const double hz = 40.0 * std::pow(400.0, i / 119.0), ratio = hz / std::max(40.0, cutoff);
        const double gain = 1.0 / std::sqrt(1.0 + std::pow(ratio, 4.0)) *
            std::max(0.25, 1.0 + res * 1.4 * std::exp(-std::pow(std::log(std::max(0.001, ratio)) / 0.28, 2.0)));
        response.push_back({fx + i * fw / 119, base - static_cast<int>(std::clamp(gain, 0.0, 2.0) * 30.0)});
      }
      g.polyline(response, amber, 2);
    } else if (page == 3) {
      const int waveform = static_cast<int>(value(kWaveform));
      const int ops = waveform == 49 ? 6 : 4;
      const int algorithm = static_cast<int>(value(kGenesisAlgorithm)) & (ops == 6 ? 31 : 7);
      g.text(r.x + 20, r.y + 24, "OPERATOR ROUTING", muted, 260, TextSize::Small);
      for (int i = 0; i < ops; ++i) {
        const int x = r.x + 40 + i * 120, y = r.y + 40;
        if (i < ops - 1 && (algorithm & (1 << std::min(i, 3))) == 0) g.line(x + 50, y + 25, x + 120, y + 25, pink, 3);
        g.circle(x, y, 50, pink);
        char op[3]{};
        std::snprintf(op, sizeof(op), "%d", i + 1);
        g.text_centered(x + 25, y + 33, op, bg, 30, TextSize::Normal);
      }
      char text_algorithm[64]{};
      std::snprintf(text_algorithm, sizeof(text_algorithm), "Algorithm %d  •  %d operators", algorithm, ops);
      g.text(r.x + 820, r.y + 70, text_algorithm, hardware_fm_waveform(waveform) || waveform == 49 ? text : faint, 600);
    } else if (page == 4) {
      g.text(r.x + 20, r.y + 24, "CHANNELS  (left mute • right solo)", muted, 700, TextSize::Small);
      const uint32_t mute = static_cast<uint32_t>(value(kStackMuteMask)), solo = static_cast<uint32_t>(value(kStackSoloMask));
      for (int i = 0; i < 16; ++i) {
        const uint32_t bit = 1U << i;
        const bool is_solo = solo & bit, is_mute = mute & bit;
        const auto tile = strip::mixer_tile(r, i);
        g.round_rect(tile, 6, is_solo ? amber : (is_mute ? 0x5a2230 : 0x1d4a3a));
        char n[4]{};
        std::snprintf(n, sizeof(n), "%02d", i + 1);
        g.text_centered(tile.x + tile.w / 2, tile.y + 20, n, is_solo ? bg : text, tile.w, TextSize::Small);
        g.text_centered(tile.x + tile.w / 2, tile.y + 39, is_solo ? "S" : (is_mute ? "M" : "ON"), is_solo ? bg : (is_mute ? red : green),
                        tile.w, TextSize::Small);
      }
      g.text(r.x + 800, r.y + 24, "DPCM SLOTS  (amber loaded • green looping)", muted, 700, TextSize::Small);
      const uint32_t loops = static_cast<uint32_t>(value(kDpcmLoopMask));
      for (int i = 0; i < 16; ++i) {
        const auto bank = p_->dpcm_banks[static_cast<size_t>(i)].load();
        const bool loaded = bank && !bank->empty(), loop = loops & (1U << i);
        const auto tile = strip::bank_tile(r, i);
        g.round_rect(tile, 6, loop ? green : (loaded ? amber : border));
        char n[3]{};
        std::snprintf(n, sizeof(n), "%X", i);
        g.text_centered(tile.x + tile.w / 2, tile.y + 29, n, loaded || loop ? bg : muted, tile.w, TextSize::Normal);
      }
    }
  }

  bool strip_pointer(int page, int button, int x, int y, const yanes::ui::Rect& r) override {
    if (page != 4) return false;
    for (int i = 0; i < 16; ++i) {
      if (strip::mixer_tile(r, i).contains(x, y) && (button == 1 || button == 3)) {
        const clap_id id = button == 3 ? kStackSoloMask : kStackMuteMask;
        const uint32_t old = static_cast<uint32_t>(value(static_cast<int>(id)));
        click(static_cast<int>(id), static_cast<double>(old ^ (1U << i)));
        return true;
      }
      if (strip::bank_tile(r, i).contains(x, y)) {
        if (button == 2) { if (gui_choose_sample(p_, i)) gui_mark_state(p_); return true; }
        if (button == 3) { install_dpcm_bank(p_, static_cast<size_t>(i), {}); gui_mark_state(p_); return true; }
        if (button == 1) {
          const uint32_t old = static_cast<uint32_t>(value(kDpcmLoopMask));
          click(kDpcmLoopMask, static_cast<double>(old ^ (1U << i)));
          return true;
        }
      }
    }
    return false;
  }

 private:
  void click(int id, double v) { begin_edit(id); edit(id, v); end_edit(id); }
  Plugin* p_;
};

// The editor outlives any one window, so page, collapsed sections, and so on survive the host
// closing and reopening it.
yanes::ui::Editor& gui_editor(Plugin* p) {
  if (!p->gui_editor) {
    p->gui_editor_host = std::make_unique<PluginEditorHost>(p);
    p->gui_editor = std::make_unique<yanes::ui::Editor>(*p->gui_editor_host, yanes::ui::yanes_pages(),
                                                        yanes::ui::yanes_header_control());
  }
  return *p->gui_editor;
}

void gui_draw(Plugin* p, yanes::ui::Canvas& canvas) { gui_editor(p).draw(canvas); }

using GuiPointer = yanes::ui::PointerAction;

void gui_input(Plugin* p, GuiPointer action, int button, int x, int y, unsigned modifiers = 0) {
  gui_editor(p).pointer(action, button, x, y, modifiers, yanes::ui::now_ms());
}

// True when the editor needs a repaint for its own animations.
bool gui_tick(Plugin* p) { return gui_editor(p).tick(yanes::ui::now_ms()); }
