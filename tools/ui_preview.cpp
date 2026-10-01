// Renders the editor offscreen through the real X11 canvas and writes one PNG per step of a
// small script, for reviewing the layout without a host. No window is ever mapped.
//
//   yanes-ui-preview OUTPUT_DIR [WIDTH HEIGHT]
//
// The host here is a stand-in: parameters start at their defaults and the page artwork is a
// placeholder, but layout, widgets, text, and interaction are the plug-in's own.
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsp.hpp"
#include "snes_dsp.hpp"
#include "params.hpp"
#include "ui_canvas_x11.hpp"
#include "ui_editor.hpp"
#include "ui_pages.hpp"

#include <cmath>

namespace {

using namespace yanes::ui;
namespace P = yanes::params;

// One cycle of the selected source oscillator for the editor's SOURCE WAVE panel,
// from the host's parameter values. Mirrors ui_source_sample in ui_frontend.hpp,
// which reads live plug-in parameters; here it reads the preview host's values.
inline float preview_source_sample(const double* v, double phase) {
  using namespace yanes::params;
  const int waveform = static_cast<int>(v[kWaveform]);
  const int shape = std::clamp(static_cast<int>(v[kExpansionShape]), 0, 7);
  if (v[kCustomWave] >= 0.5 || waveform == 58) {
    const int i = std::clamp(static_cast<int>(phase * 32.0), 0, 31);
    return static_cast<float>(v[kWaveSample1 + i] / 7.5 - 1.0);
  }
  switch (waveform) {
    case 0: case 10: case 18: case 19:
      return yanes::pulse_raw(phase, kDuties[std::clamp(static_cast<int>(v[kDuty]), 0, 3)]);
    case 8: case 13: case 15: case 22: case 23: case 24: case 42: case 44:
      return yanes::pulse_raw(phase, 0.5);
    case 1: return yanes::nes_triangle(phase);
    case 3: return yanes::pulse_raw(phase, (shape + 1) / 16.0);
    case 4: return yanes::vrc6_saw(phase, 42);
    case 5: return yanes::fds_wave(phase, shape);
    case 6: case 11: return yanes::n163_wave(phase, shape);
    case 7: return yanes::vrc7_fm(phase, v[kFmRatio], v[kFmIndex]);
    case 26: case 35: return yanes::pce_wave(phase, shape);
    case 38: case 39: return static_cast<float>(2.0 * phase - 1.0);
    case 40: case 41: return yanes::scc_wave(phase, shape);
    case 46: return yanes::morph_wavetable(phase, v[kWavetablePosition], v[kWavetableWarp]);
    case 47: return yanes::phase_distortion(phase, v[kWavetablePosition], v[kWavetableWarp]);
    case 48: return yanes::additive(phase, v[kAdditiveTilt], v[kWavetablePosition]);
    case 49: return yanes::six_operator_fm(phase, static_cast<int>(v[kGenesisAlgorithm]), v[kFmIndex], v[kFmBrightness]);
    case 50: return yanes::morph_wavetable(phase, v[kWavetablePosition], 0.5);
    case 51: return yanes::porta_fm(phase, v[kFmRatio], v[kFmIndex], v[kFmBrightness]);
    case 52: case 53: return yanes::analog_poly(phase, std::fmod(phase * 1.01, 1.0), shape / 7.0);
    case 54: return yanes::digital_ensemble(phase, v[kWavetablePosition]);
    case 55: return yanes::tine_piano(phase, v[kFmIndex], v[kFmBrightness], 0.0);
    case 56: {
      const double saw = phase * 2.0 - 1.0, sub = std::fmod(phase * 0.5, 1.0) < 0.5 ? 1.0 : -1.0;
      return static_cast<float>(std::tanh((saw * 0.78 + sub * 0.22) * 1.4));
    }
    case 59: case 63: return yanes::snes_wave(phase, shape);
    case 60: return yanes::bell_fm(phase, v[kFmRatio], v[kFmIndex], 0.0);
    case 61: return yanes::feedback_fm(phase, v[kFmIndex], v[kFmBrightness]);
    case 62: return yanes::formant_wave(phase, v[kWavetablePosition], v[kWavetableWarp]);
    default: break;
  }
  if (waveform == 2 || waveform == 12 || waveform == 14 || waveform == 16 || waveform == 37) {
    const uint32_t s = static_cast<uint32_t>(phase * 64.0) * 2654435761u;
    return static_cast<float>(((s >> 8) & 0xffu) / 127.5 - 1.0);
  }
  return yanes::genesis_fm(phase, static_cast<int>(v[kGenesisAlgorithm]), v[kGenesisFeedback]);
}

inline bool preview_hardware_fm(int w) { return w == 17 || (w >= 27 && w <= 33) || w == 21 || w == 36; }
inline Rect preview_mixer_tile(const Rect& a, int c) { return {a.x + 20 + c * 46, a.y + 34, 40, 44}; }
inline Rect preview_bank_tile(const Rect& a, int s) { return {a.x + 800 + s * 44, a.y + 34, 38, 44}; }

class PreviewHost final : public EditorHost {
 public:
  PreviewHost() { for (int i = 0; i < P::kParamCount; ++i) values[static_cast<size_t>(i)] = P::kSpecs[static_cast<size_t>(i)].def; }
  std::array<double, P::kParamCount> values{};
  int param_count() const override { return P::kParamCount; }
  ParamInfo info(int id) const override {
    const auto& s = P::kSpecs[static_cast<size_t>(id)];
    return {s.name, s.min, s.max, s.def, s.stepped};
  }
  double value(int id) const override { return values[static_cast<size_t>(id)]; }
  void begin_edit(int) override {}
  void edit(int id, double v) override {
    const auto& s = P::kSpecs[static_cast<size_t>(id)];
    v = std::clamp(v, s.min, s.max);
    values[static_cast<size_t>(id)] = s.stepped ? std::round(v) : v;
  }
  void end_edit(int) override {}
  void format(int id, double v, char* out, size_t n) const override { P::format_value(static_cast<uint32_t>(id), v, out, static_cast<uint32_t>(n)); }
  const char* short_label(int id) const override {
    if (const char* name = yanes_short_name(id)) return name;
    return label(id);
  }
  bool relevant(int id) const override {
    const int wave = static_cast<int>(values[P::kWaveform]);
    if (id >= P::kFmAttack && id <= P::kFmPmDepth) return wave == 17;
    if (id >= P::kSequence1 && id <= P::kSequence8) return static_cast<int>(values[P::kArpMode]) == 5;
    if (id == P::kFmRatio || id == P::kFmIndex || id == P::kNoisePeriod) return false;
    return true;
  }
  int size_percent() const override { return percent; }
  int percent = 100;
  // The plug-in's own page artwork, rendered from parameter values. The live
  // output scope and spectrum have no audio here, so they read as a resting
  // signal; everything else (the source-wave cycle, envelope sketch, filter
  // response, operator routing, channel and sample tiles) is exact.
  void draw_strip(int page, Painter& g, const Rect& r) override {
    using namespace yanes::ui::theme;
    const double* v = values.data();
    if (page == 0) {
      const int mid = r.y + r.h / 2 + 10, amp = 52;
      const int src_x = r.x + 20, src_w = 380;
      g.text(src_x, r.y + 24, "SOURCE WAVE", muted, 200, TextSize::Small);
      g.line(src_x, mid, src_x + src_w, mid, border);
      std::vector<Point> source;
      for (int i = 0; i < 160; ++i)
        source.push_back({src_x + i * src_w / 159,
                          mid - static_cast<int>(std::clamp(preview_source_sample(v, i / 159.0), -1.15f, 1.15f) * amp)});
      g.polyline(source, cyan, 3);
      const int scope_x = r.x + 430, scope_w = 380;
      g.text(scope_x, r.y + 24, "OUTPUT SCOPE", muted, 200, TextSize::Small);
      g.line(scope_x, mid, scope_x + scope_w, mid, border);
      g.polyline({{scope_x, mid}, {scope_x + scope_w, mid}}, green, 2);
      const int spectrum_x = r.x + 840, base = r.bottom() - 16;
      g.text(spectrum_x, r.y + 24, "SPECTRUM", muted, 180, TextSize::Small);
      for (int band = 0; band < 32; ++band) g.rect(spectrum_x + band * 15, base - 2, 10, 2, band < 22 ? green : amber);
      const double attack = value(P::kAttackMs), release = value(P::kReleaseMs);
      const int ex = r.x + 1340, top = r.y + 42, bottom = r.bottom() - 18;
      g.text(ex, r.y + 24, "ENVELOPE", muted, 150, TextSize::Small);
      const int ax = ex + static_cast<int>(std::clamp(attack / 500.0, 0.0, 1.0) * 60.0);
      const int sx = ex + 110, rx = sx + static_cast<int>(std::clamp(release / 2000.0, 0.0, 1.0) * 70.0) + 4;
      g.polyline({{ex, bottom}, {ax, top}, {sx, top}, {rx, bottom}}, green, 2);
    } else if (page == 2) {
      const int wx = r.x + 20, ww = 900, mid = r.y + r.h / 2 + 10, amp = 56;
      g.text(wx, r.y + 24, "SOURCE WAVE", muted, 200, TextSize::Small);
      g.line(wx, mid, wx + ww, mid, border);
      std::vector<Point> points;
      for (int i = 0; i < 200; ++i)
        points.push_back({wx + i * ww / 199,
                          mid - static_cast<int>(std::clamp(preview_source_sample(v, i / 199.0), -1.2f, 1.2f) * amp)});
      g.polyline(points, green, 3);
      const int fx = r.x + 980, fw = 520, base = r.bottom() - 16;
      g.text(fx, r.y + 24, "FILTER RESPONSE", muted, 240, TextSize::Small);
      const double cutoff = value(P::kChipCutoff), res = value(P::kChipResonance);
      std::vector<Point> response;
      for (int i = 0; i < 120; ++i) {
        const double hz = 40.0 * std::pow(400.0, i / 119.0), ratio = hz / std::max(40.0, cutoff);
        const double gain = 1.0 / std::sqrt(1.0 + std::pow(ratio, 4.0)) *
            std::max(0.25, 1.0 + res * 1.4 * std::exp(-std::pow(std::log(std::max(0.001, ratio)) / 0.28, 2.0)));
        response.push_back({fx + i * fw / 119, base - static_cast<int>(std::clamp(gain, 0.0, 2.0) * 40.0)});
      }
      g.polyline(response, amber, 2);
    } else if (page == 3) {
      const int waveform = static_cast<int>(value(P::kWaveform));
      const int ops = waveform == 49 ? 6 : 4;
      const int algorithm = static_cast<int>(value(P::kGenesisAlgorithm)) & (ops == 6 ? 31 : 7);
      g.text(r.x + 20, r.y + 24, "OPERATOR ROUTING", muted, 260, TextSize::Small);
      for (int i = 0; i < ops; ++i) {
        const int x = r.x + 40 + i * 120, y = r.y + 40;
        if (i < ops - 1 && (algorithm & (1 << std::min(i, 3))) == 0) g.line(x + 50, y + 25, x + 120, y + 25, pink, 3);
        g.circle(x, y, 50, pink);
        char op[16]{}; std::snprintf(op, sizeof(op), "%d", i + 1);
        g.text_centered(x + 25, y + 33, op, bg, 30, TextSize::Normal);
      }
      char text_algorithm[64]{};
      std::snprintf(text_algorithm, sizeof(text_algorithm), "Algorithm %d  \xe2\x80\xa2  %d operators", algorithm, ops);
      g.text(r.x + 820, r.y + 70, text_algorithm, preview_hardware_fm(waveform) || waveform == 49 ? text : faint, 600);
    } else if (page == 4) {
      g.text(r.x + 20, r.y + 24, "CHANNELS  (left mute \xe2\x80\xa2 right solo)", muted, 700, TextSize::Small);
      const uint32_t mute = static_cast<uint32_t>(value(P::kStackMuteMask)), solo = static_cast<uint32_t>(value(P::kStackSoloMask));
      for (int i = 0; i < 16; ++i) {
        const bool is_solo = solo & (1U << i), is_mute = mute & (1U << i);
        const auto tile = preview_mixer_tile(r, i);
        g.round_rect(tile, 6, is_solo ? amber : (is_mute ? 0x5a2230 : 0x1d4a3a));
        char n[4]{}; std::snprintf(n, sizeof(n), "%02d", i + 1);
        g.text_centered(tile.x + tile.w / 2, tile.y + 20, n, is_solo ? bg : text, tile.w, TextSize::Small);
        g.text_centered(tile.x + tile.w / 2, tile.y + 39, is_solo ? "S" : (is_mute ? "M" : "ON"), is_solo ? bg : (is_mute ? red : green), tile.w, TextSize::Small);
      }
      g.text(r.x + 800, r.y + 24, "DPCM SLOTS  (amber loaded \xe2\x80\xa2 green looping)", muted, 700, TextSize::Small);
      const uint32_t loops = static_cast<uint32_t>(value(P::kDpcmLoopMask));
      for (int i = 0; i < 16; ++i) {
        const bool loop = loops & (1U << i);
        const auto tile = preview_bank_tile(r, i);
        g.round_rect(tile, 6, loop ? green : border);
        char n[3]{}; std::snprintf(n, sizeof(n), "%X", i);
        g.text_centered(tile.x + tile.w / 2, tile.y + 29, n, loop ? bg : muted, tile.w, TextSize::Normal);
      }
    }
  }
  void draw_meters(Painter& g, const Rect& r) override {
    g.text(r.x, r.y + 24, "OUT", theme::muted, 50, TextSize::Small);
    g.rect(r.x + 52, r.y + 6, 230, 9, theme::border);
    g.rect(r.x + 52, r.y + 6, 140, 9, theme::green);
    g.rect(r.x + 52, r.y + 20, 230, 9, theme::border);
    g.rect(r.x + 52, r.y + 20, 120, 9, theme::green);
  }
};

struct Surface {
  Display* display{};
  Pixmap pixmap{};
  GC gc{};
  XftDraw* xft{};
  std::array<XftFont*, 3> fonts{};
  int w{}, h{};
};

bool write_png(Surface& s, const std::string& path) {
  XImage* image = XGetImage(s.display, s.pixmap, 0, 0, static_cast<unsigned>(s.w), static_cast<unsigned>(s.h), AllPlanes, ZPixmap);
  if (!image) return false;
  const std::string ppm = path + ".ppm";
  FILE* f = std::fopen(ppm.c_str(), "wb");
  if (!f) { XDestroyImage(image); return false; }
  std::fprintf(f, "P6\n%d %d\n255\n", s.w, s.h);
  for (int y = 0; y < s.h; ++y)
    for (int x = 0; x < s.w; ++x) {
      const unsigned long pixel = XGetPixel(image, x, y);
      const unsigned char rgb[3]{static_cast<unsigned char>((pixel >> 16) & 255), static_cast<unsigned char>((pixel >> 8) & 255),
                                 static_cast<unsigned char>(pixel & 255)};
      std::fwrite(rgb, 1, 3, f);
    }
  std::fclose(f);
  XDestroyImage(image);
  const std::string command = "magick '" + ppm + "' '" + path + "' && rm -f '" + ppm + "'";
  return std::system(command.c_str()) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s OUTPUT_DIR [WIDTH HEIGHT]\n", argv[0]); return 2; }
  const std::string out = argv[1];
  Surface s;
  s.w = argc >= 4 ? std::atoi(argv[2]) : width;
  s.h = argc >= 4 ? std::atoi(argv[3]) : height;
  s.display = XOpenDisplay(nullptr);
  if (!s.display) { std::fprintf(stderr, "no X display\n"); return 1; }
  initialize_x11_text(s.display);
  const int screen = DefaultScreen(s.display);
  s.pixmap = XCreatePixmap(s.display, DefaultRootWindow(s.display), static_cast<unsigned>(s.w), static_cast<unsigned>(s.h),
                           static_cast<unsigned>(DefaultDepth(s.display, screen)));
  s.gc = XCreateGC(s.display, s.pixmap, 0, nullptr);
  s.xft = XftDrawCreate(s.display, s.pixmap, DefaultVisual(s.display, screen), DefaultColormap(s.display, screen));
  for (int i = 0; i < 3; ++i) {
    char pattern[128]{};
    std::snprintf(pattern, sizeof(pattern), "DejaVu Sans:weight=%s:pixelsize=%d", i == 2 ? "bold" : "medium",
                  font_pixels(static_cast<TextSize>(i), s.w, s.h));
    s.fonts[static_cast<size_t>(i)] = XftFontOpenName(s.display, screen, pattern);
  }
  PreviewHost host;
  host.percent = static_cast<int>(std::lround(100.0 * uniform_scale(s.w, s.h)));
  host.values[P::kArpMode] = 5;
  host.values[P::kCentsSeqMode] = 1;
  host.values[P::kCentsStep1] = 50; host.values[P::kCentsStep2] = -30; host.values[P::kCentsStep3] = 100;
  host.values[P::kDutySeqMode] = 1;
  host.values[P::kSequence2] = 7; host.values[P::kSequence3] = 12; host.values[P::kSequence4] = -5;
  Editor editor(host, yanes_pages(), yanes_header_control());
  double now = 0.0;
  const auto settle = [&] { for (int i = 0; i < 40; ++i) editor.tick(now += 16.0); };
  const auto snap = [&](const std::string& name) {
    X11Canvas canvas(s.display, s.pixmap, s.gc, s.xft, s.fonts, s.w, s.h);
    editor.draw(canvas);
    XSync(s.display, False);
    if (!write_png(s, out + "/" + name + ".png")) std::fprintf(stderr, "failed to write %s\n", name.c_str());
  };
  const auto sx = [&](int x) { return x; };
  for (int page = 0; page < editor.page_count(); ++page) {
    editor.set_page(page);
    settle();
    char name[32]{};
    std::snprintf(name, sizeof(name), "page%d", page + 1);
    snap(name);
  }
  // Hover a knob, open the waveform menu, collapse a section, and catch a transition midway.
  editor.set_page(0);
  settle();
  const PageLayout lay = editor.layout();
  const Rect knob = lay.controls[4].rect;
  editor.pointer(PointerAction::Move, 0, sx(knob.x + knob.w / 2), knob.y + 60, 0, now);
  snap("hover");
  const Rect wave = lay.controls[0].rect;
  editor.pointer(PointerAction::Down, 1, wave.x + 40, wave.y + 60, 0, now);
  editor.pointer(PointerAction::Up, 1, wave.x + 40, wave.y + 60, 0, now);
  settle();
  snap("menu");
  editor.pointer(PointerAction::Down, 1, 5, 5, 0, now);
  editor.pointer(PointerAction::Up, 1, 5, 5, 0, now);
  settle();
  // Open the preset browser in the header.
  editor.pointer(PointerAction::Down, 1, preset_rect.x + 120, preset_rect.y + preset_rect.h / 2, 0, now);
  editor.pointer(PointerAction::Up, 1, preset_rect.x + 120, preset_rect.y + preset_rect.h / 2, 0, now);
  settle();
  snap("presets");
  editor.pointer(PointerAction::Down, 1, 5, 5, 0, now);
  editor.pointer(PointerAction::Up, 1, 5, 5, 0, now);
  settle();
  editor.set_section_open(0, 1, false);
  settle();
  snap("collapsed");
  editor.set_page(1);
  for (int i = 0; i < 4; ++i) editor.tick(now += 16.0);
  snap("transition");
  XftDrawDestroy(s.xft);
  for (XftFont* font : s.fonts) if (font) XftFontClose(s.display, font);
  XFreeGC(s.display, s.gc);
  XFreePixmap(s.display, s.pixmap);
  XCloseDisplay(s.display);
  return 0;
}
