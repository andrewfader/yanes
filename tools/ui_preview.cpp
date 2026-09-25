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

#include "params.hpp"
#include "ui_canvas_x11.hpp"
#include "ui_editor.hpp"
#include "ui_pages.hpp"

namespace {

using namespace yanes::ui;
namespace P = yanes::params;

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
  void draw_strip(int page, Painter& g, const Rect& r) override {
    char label[64]{};
    std::snprintf(label, sizeof(label), "page %d artwork (drawn by the plug-in)", page + 1);
    g.text(r.x + 20, r.y + r.h / 2 + 8, label, theme::faint, 800, TextSize::Small);
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
