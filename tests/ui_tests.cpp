// Editor coverage without a window: the real page table and editor, driven by a fake host that
// enforces CLAP's gesture rules and a canvas that records what is drawn.
#include "params.hpp"
#include "ui_editor.hpp"
#include "ui_layout.hpp"
#include "ui_pages.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace yanes::ui;
namespace P = yanes::params;
constexpr int kParams = static_cast<int>(P::kParamCount);

// Checks every edit against CLAP's rule that a change from the editor sits inside a gesture
// for that parameter, and that gestures never nest or leak.
class FakeHost final : public EditorHost {
 public:
  FakeHost() { for (int i = 0; i < kParams; ++i) values[static_cast<size_t>(i)] = P::kSpecs[static_cast<size_t>(i)].def; }
  std::array<double, P::kParamCount> values{};
  std::set<int> open;
  std::vector<std::string> log;
  int begins = 0, edits = 0, ends = 0;
  std::vector<int> strip_clicks;

  int param_count() const override { return P::kParamCount; }
  ParamInfo info(int id) const override {
    assert(id >= 0 && id < kParams);
    const auto& s = P::kSpecs[static_cast<size_t>(id)];
    return {s.name, s.min, s.max, s.def, s.stepped};
  }
  double value(int id) const override { return values[static_cast<size_t>(id)]; }
  void begin_edit(int id) override {
    assert(!open.count(id) && "gesture begun twice");
    open.insert(id);
    ++begins;
  }
  void edit(int id, double v) override {
    assert(open.count(id) && "edit outside a gesture");
    const auto& s = P::kSpecs[static_cast<size_t>(id)];
    assert(v >= s.min - 1e-9 && v <= s.max + 1e-9 && "edit out of range");
    if (s.stepped) assert(v == std::round(v) && "stepped parameter edited to a fraction");
    values[static_cast<size_t>(id)] = v;
    ++edits;
  }
  void end_edit(int id) override {
    assert(open.count(id) && "gesture ended without a begin");
    open.erase(id);
    ++ends;
  }
  void format(int id, double v, char* out, size_t n) const override {
    P::format_value(static_cast<uint32_t>(id), v, out, static_cast<uint32_t>(n));
  }
  const char* short_label(int id) const override {
    const char* name = yanes_short_name(id);
    return name ? name : label(id);
  }
  bool strip_pointer(int page, int button, int, int, const Rect&) override {
    strip_clicks.push_back(page * 10 + button);
    return true;
  }
  void reset_counts() { begins = edits = ends = 0; }
  bool idle() const { return open.empty(); }
};

// Records drawing and checks it stays inside the design space.
class RecordingCanvas final : public Canvas {
 public:
  int rects = 0, lines = 0, circles = 0, texts = 0;
  std::vector<std::string> strings;
  void clear(uint32_t) override {}
  void fill_rect(int x, int y, int w, int h, uint32_t) override {
    ++rects;
    check(x, y); check(x + w, y + h);
  }
  void draw_polyline(const Point* points, int count, uint32_t, int thickness) override {
    ++lines;
    assert(count >= 2 && thickness >= 1);
    for (int i = 0; i < count; ++i) check(points[i].x, points[i].y);
  }
  void fill_circle(int x, int y, int d, uint32_t) override {
    ++circles;
    check(x, y); check(x + d, y + d);
  }

 protected:
  void draw_glyphs(int x, int y, const std::string& text, uint32_t, TextSize size) override {
    ++texts;
    strings.push_back(text);
    check(x, y);
    check(x + measure_text(text, size), y);
  }
  // Roughly proportional-font widths, so ellipsis fitting is exercised.
  int measure_text(const std::string& text, TextSize size) override {
    return static_cast<int>(text.size()) * text_pixels(size) * 55 / 100;
  }

 private:
  static void check(int x, int y) {
    // Page transitions slide content a few units, and menu shadows sit just outside a menu.
    if (x < -2 || x > width + 16 || y < -16 || y > height + 16) {
      std::fprintf(stderr, "drawing outside the editor at (%d, %d)\n", x, y);
      assert(false);
    }
  }
};

struct Rig {
  FakeHost host;
  Editor editor{host, yanes_pages(), yanes_header_control()};
  double now = 1000.0;
  void settle() { for (int i = 0; i < 60; ++i) editor.tick(now += 16.0); }
  void move(int x, int y, unsigned mods = 0) { editor.pointer(PointerAction::Move, 0, x, y, mods, now); }
  void down(int button, int x, int y, unsigned mods = 0) { editor.pointer(PointerAction::Down, button, x, y, mods, now); }
  void up(int button, int x, int y, unsigned mods = 0) { editor.pointer(PointerAction::Up, button, x, y, mods, now); }
  void click(int button, int x, int y, unsigned mods = 0) { down(button, x, y, mods); up(button, x, y, mods); now += 500.0; }
  void wheel(int direction, int x, int y, unsigned mods = 0) { down(direction > 0 ? 4 : 5, x, y, mods); }

  // The placed control on the current page for a parameter (a lane is found by its first step).
  PlacedControl find(int param) const {
    const PageLayout lay = editor.layout();
    for (const PlacedControl& placed : lay.controls)
      if (editor.page_spec(editor.page()).sections[static_cast<size_t>(placed.section)]
              .controls[static_cast<size_t>(placed.control)].param == param)
        return placed;
    std::fprintf(stderr, "parameter %d is not on page %d\n", param, editor.page());
    assert(false);
    return {};
  }
  void go_to(int param) {
    for (int page = 0; page < editor.page_count(); ++page)
      for (const SectionSpec& section : editor.page_spec(page).sections)
        for (const ControlSpec& control : section.controls)
          if (control.param == param) { editor.set_page(page); settle(); return; }
    assert(false && "parameter is on no page");
  }
};

// --- scaling --------------------------------------------------------------------------------------

void test_scaling() {
  for (const auto size : std::array<std::array<int, 2>, 5>{{{960, 630}, {1280, 720}, {1600, 1050}, {1920, 1200}, {2200, 900}}}) {
    for (const int x : {0, margin, 700, width}) assert(std::abs(unscale_x(scale_x(x, size[0]), size[0]) - x) <= 1);
    for (const int y : {0, tab_y, strip_y, height}) assert(std::abs(unscale_y(scale_y(y, size[1]), size[1]) - y) <= 1);
    for (const TextSize t : {TextSize::Small, TextSize::Normal, TextSize::Large})
      assert(font_pixels(t, size[0], size[1]) >= 11);
  }
  assert(font_pixels(TextSize::Normal, width, height) == text_pixels(TextSize::Normal));
  assert(font_pixels(TextSize::Normal, width * 2, height * 2) == 2 * text_pixels(TextSize::Normal));
  assert(mix(0x000000, 0xffffff, 0.0) == 0x000000 && mix(0x000000, 0xffffff, 1.0) == 0xffffff);
  assert(mix(0x102030, 0x102030, 0.5) == 0x102030);
}

// --- page table -----------------------------------------------------------------------------------

// Every parameter is reachable from exactly one place: a card control, a lane step, the header,
// or page artwork.
void test_every_parameter_is_placed_once() {
  std::vector<int> seen(static_cast<size_t>(kParams), 0);
  const auto pages = yanes_pages();
  for (const PageSpec& page : pages)
    for (const SectionSpec& section : page.sections)
      for (const ControlSpec& control : section.controls) {
        if (control.widget == Widget::Lane) {
          assert(control.steps > 0 && control.length >= 0);
          for (int i = 0; i < control.steps; ++i) ++seen[static_cast<size_t>(control.param + i)];
        } else {
          ++seen[static_cast<size_t>(control.param)];
        }
      }
  ++seen[static_cast<size_t>(yanes_header_control().param)];
  for (const int id : yanes_strip_params()) ++seen[static_cast<size_t>(id)];
  for (int id = 0; id < kParams; ++id)
    if (seen[static_cast<size_t>(id)] != 1) {
      std::fprintf(stderr, "parameter %d ('%s') is placed %d times\n", id, P::kSpecs[static_cast<size_t>(id)].name, seen[static_cast<size_t>(id)]);
      assert(false);
    }
}

// Widget choice has to suit the parameter: toggles are two-valued, segments are short lists of
// options, knobs and menus cover the rest.
void test_widgets_match_parameters() {
  for (const PageSpec& page : yanes_pages())
    for (const SectionSpec& section : page.sections)
      for (const ControlSpec& c : section.controls) {
        const auto& s = P::kSpecs[static_cast<size_t>(c.param)];
        switch (c.widget) {
          case Widget::Toggle: assert(s.stepped && s.max - s.min == 1.0); break;
          case Widget::Segments: assert(s.stepped && s.max - s.min >= 1.0 && s.max - s.min <= 4.0); break;
          case Widget::Menu: assert(s.stepped && s.max - s.min >= 2.0); break;
          case Widget::Lane: {
            assert(s.stepped);
            const auto& length = P::kSpecs[static_cast<size_t>(c.length)];
            assert(length.min == 1.0 && length.max == c.steps);
            for (int i = 1; i < c.steps; ++i) {
              const auto& step = P::kSpecs[static_cast<size_t>(c.param + i)];
              assert(step.min == s.min && step.max == s.max);
            }
            break;
          }
          case Widget::Knob: break;
        }
      }
}

// With everything open, every page fits between the tabs and the footer, nothing overlaps, and
// every control sits inside its own card.
void test_pages_fit_open() {
  const auto pages = yanes_pages();
  for (size_t p = 0; p < pages.size(); ++p) {
    const PageLayout lay = layout_page(pages[p], std::vector<double>(pages[p].sections.size(), 1.0));
    size_t controls = 0;
    for (const SectionSpec& s : pages[p].sections) controls += s.controls.size();
    assert(lay.controls.size() == controls && "an open card hides a control");
    for (size_t i = 0; i < lay.sections.size(); ++i) {
      const Rect& card = lay.sections[i].card;
      assert(card.x >= margin && card.right() <= width - margin);
      assert(card.y >= lay.content_top && card.bottom() <= content_bottom);
      for (size_t j = i + 1; j < lay.sections.size(); ++j) assert(!card.overlaps(lay.sections[j].card));
    }
    for (size_t i = 0; i < lay.controls.size(); ++i) {
      const Rect& r = lay.controls[i].rect;
      const Rect& card = lay.sections[static_cast<size_t>(lay.controls[i].section)].card;
      assert(r.x >= card.x && r.right() <= card.right() && r.y >= card.y + card_header && r.bottom() <= card.bottom());
      for (size_t j = i + 1; j < lay.controls.size(); ++j) assert(!r.overlaps(lay.controls[j].rect));
    }
    if (pages[p].strip_height > 0) assert(lay.strip.bottom() < lay.content_top);
  }
  for (int i = 0; i < tab_count; ++i) {
    assert(tab_at(tab_rect(i).x + 5, tab_y + 5) == i);
    assert(tab_rect(i).right() <= width - margin);
  }
  assert(tab_at(margin - 1, tab_y + 5) == -1 && tab_at(margin + 5, tab_y - 1) == -1);
  assert(!preset_rect.overlaps(meter_rect) && preset_rect.bottom() < tab_y && content_bottom < footer_rect.y);
}

// --- navigation and animation ---------------------------------------------------------------------

void test_tabs_switch_and_settle() {
  Rig rig;
  assert(rig.editor.page() == 0);
  rig.move(tab_rect(3).x + 20, tab_y + 20);
  rig.click(1, tab_rect(3).x + 20, tab_y + 20);
  assert(rig.editor.page() == 3);
  assert(rig.editor.tick(rig.now += 16.0) && "a page change animates");
  rig.settle();
  assert(!rig.editor.tick(rig.now += 16.0) && "animations come to rest");
  // Clicking the current tab changes nothing.
  rig.click(1, tab_rect(3).x + 20, tab_y + 20);
  assert(!rig.editor.tick(rig.now += 16.0));
  // The transition takes about the documented time regardless of when the last tick was.
  rig.editor.set_page(1);
  rig.now += 60000.0;
  int frames = 0;
  while (rig.editor.tick(rig.now += 16.0)) ++frames;
  assert(frames >= 5 && frames <= 30);
  assert(rig.host.begins == 0 && "navigation is not a parameter edit");
}

void test_sections_collapse_and_reflow() {
  Rig rig;
  const PageLayout before = rig.editor.layout();
  // Envelope and Level share a column on the voice page.
  const auto& sections = rig.editor.page_spec(0).sections;
  assert(sections[1].column == sections[2].column);
  const Rect header = before.sections[1].header;
  rig.click(1, header.x + 60, header.y + 10);
  assert(!rig.editor.section_open(0, 1));
  double last = 1.0;
  for (int i = 0; i < 40; ++i) {
    rig.editor.tick(rig.now += 16.0);
    const double open = rig.editor.section_openness(0, 1);
    assert(open <= last);
    last = open;
  }
  assert(last == 0.0);
  const PageLayout after = rig.editor.layout();
  assert(after.sections[1].card.h == card_header);
  assert(after.sections[2].card.y < before.sections[2].card.y && "the card below moves up");
  for (const PlacedControl& c : after.controls) assert(c.section != 1 && "a closed card shows no controls");
  // Collapsed state is per page and survives switching away and back.
  rig.editor.set_page(2); rig.settle();
  rig.editor.set_page(0); rig.settle();
  assert(!rig.editor.section_open(0, 1));
  rig.click(1, header.x + 60, header.y + 10);
  rig.settle();
  assert(rig.editor.layout().sections[1].card.h == before.sections[1].card.h);
}

// --- knobs ----------------------------------------------------------------------------------------

void test_knob_drag() {
  Rig rig;
  const PlacedControl attack = rig.find(P::kAttackMs);
  const int cx = attack.rect.x + attack.rect.w / 2, cy = attack.rect.y + 60;
  const auto& spec = P::kSpecs[P::kAttackMs];
  rig.host.values[P::kAttackMs] = spec.min;
  rig.down(1, cx, cy);
  assert(rig.editor.dragging() == P::kAttackMs && rig.host.open.count(P::kAttackMs));
  // A full drag range upward sweeps the whole range, in several moves, clamping at the top.
  for (int i = 1; i <= 10; ++i) rig.move(cx + i * 3, cy - static_cast<int>(Editor::drag_range) * i / 10);
  assert(std::abs(rig.host.values[P::kAttackMs] - spec.max) < 1e-9);
  rig.move(cx, cy - 900);
  assert(rig.host.values[P::kAttackMs] == spec.max);
  // Downward from the top brings it back; horizontal movement does not matter.
  rig.move(cx + 200, cy - 900 + 125);
  assert(std::abs(rig.host.values[P::kAttackMs] - (spec.max - (spec.max - spec.min) / 2.0)) < 1e-6);
  rig.up(1, cx, cy);
  assert(rig.host.idle() && rig.host.begins == 1 && rig.host.ends == 1);

  // Shift makes the same movement a tenth as large.
  rig.now += 1000.0;  // not a double-click
  rig.host.values[P::kAttackMs] = spec.min;
  rig.down(1, cx, cy);
  rig.move(cx, cy - 100, kShift);
  rig.up(1, cx, cy - 100, kShift);
  assert(std::abs(rig.host.values[P::kAttackMs] - (spec.max - spec.min) * 100.0 / Editor::drag_range * Editor::fine_factor) < 1e-6);
  assert(rig.host.idle());
}

void test_stepped_knob_snaps() {
  Rig rig;
  const PlacedControl rate = rig.find(P::kEnvelopeRate);
  const int cx = rate.rect.x + rate.rect.w / 2, cy = rate.rect.y + 60;
  rig.host.values[P::kEnvelopeRate] = 0;
  rig.down(1, cx, cy);
  // Small movements accumulate rather than being lost to rounding.
  for (int i = 1; i <= 40; ++i) rig.move(cx, cy - i);
  rig.up(1, cx, cy - 40);
  const double expected = std::round(40.0 / Editor::drag_range * 15.0);
  assert(rig.host.values[P::kEnvelopeRate] == expected && expected > 0);
}

void test_knob_resets() {
  Rig rig;
  const PlacedControl release = rig.find(P::kReleaseMs);
  const int cx = release.rect.x + release.rect.w / 2, cy = release.rect.y + 60;
  const double def = P::kSpecs[P::kReleaseMs].def;
  // Right-click.
  rig.host.values[P::kReleaseMs] = 900;
  rig.click(3, cx, cy);
  assert(rig.host.values[P::kReleaseMs] == def && rig.host.idle());
  // Ctrl-click.
  rig.host.values[P::kReleaseMs] = 900;
  rig.click(1, cx, cy, kControl);
  assert(rig.host.values[P::kReleaseMs] == def && rig.host.idle());
  // Double-click: two presses close together, without moving.
  rig.host.values[P::kReleaseMs] = 900;
  rig.down(1, cx, cy); rig.up(1, cx, cy);
  rig.now += 120.0;
  rig.down(1, cx, cy); rig.up(1, cx, cy);
  assert(rig.host.values[P::kReleaseMs] == def && rig.host.idle());
  // Two presses too far apart in time are two drags, not a reset.
  rig.host.values[P::kReleaseMs] = 900;
  rig.now += 1000.0;
  rig.down(1, cx, cy); rig.up(1, cx, cy);
  rig.now += Editor::double_click_ms + 50.0;
  rig.down(1, cx, cy); rig.up(1, cx, cy);
  assert(rig.host.values[P::kReleaseMs] == 900);
  // A triple click resets once, then starts a new drag.
  rig.now += 1000.0;
  rig.down(1, cx, cy); rig.up(1, cx, cy); rig.now += 100.0;
  rig.down(1, cx, cy); rig.up(1, cx, cy); rig.now += 100.0;
  rig.down(1, cx, cy);
  assert(rig.editor.dragging() == P::kReleaseMs);
  rig.up(1, cx, cy);
  assert(rig.host.idle());
}

void test_wheel() {
  Rig rig;
  const PlacedControl attack = rig.find(P::kAttackMs);
  const int cx = attack.rect.x + attack.rect.w / 2, cy = attack.rect.y + 60;
  const auto& spec = P::kSpecs[P::kAttackMs];
  rig.host.values[P::kAttackMs] = 100;
  rig.wheel(1, cx, cy);
  assert(std::abs(rig.host.values[P::kAttackMs] - (100 + (spec.max - spec.min) * Editor::wheel_fraction)) < 1e-9);
  rig.wheel(-1, cx, cy, kShift);
  assert(std::abs(rig.host.values[P::kAttackMs] - (100 + (spec.max - spec.min) * Editor::wheel_fraction * (1 - Editor::fine_factor))) < 1e-9);
  assert(rig.host.idle() && rig.host.begins == 2 && rig.host.ends == 2);
  // Stepped: one step per notch, clamped.
  const PlacedControl rate = rig.find(P::kEnvelopeRate);
  rig.host.values[P::kEnvelopeRate] = 15;
  rig.wheel(1, rate.rect.x + 20, rate.rect.y + 60);
  assert(rig.host.values[P::kEnvelopeRate] == 15);
  rig.wheel(-1, rate.rect.x + 20, rate.rect.y + 60);
  assert(rig.host.values[P::kEnvelopeRate] == 14);
  // Wheel over empty space does nothing.
  rig.host.reset_counts();
  rig.wheel(1, width / 2, content_bottom - 2);
  assert(rig.host.begins == 0);
}

// --- choices --------------------------------------------------------------------------------------

void test_toggle_and_segments() {
  Rig rig;
  const PlacedControl velocity = rig.find(P::kVelocity);
  const double before = rig.host.values[P::kVelocity];
  rig.click(1, velocity.rect.x + velocity.rect.w / 2, velocity.rect.y + 60);
  assert(rig.host.values[P::kVelocity] == 1.0 - before && rig.host.idle());
  rig.click(1, velocity.rect.x + velocity.rect.w / 2, velocity.rect.y + 60);
  assert(rig.host.values[P::kVelocity] == before);

  // Each segment selects its own option.
  const PlacedControl duty = rig.find(P::kDuty);
  for (int option = 0; option < 4; ++option) {
    const int x = duty.rect.x + 8 + (duty.rect.w - 16) * (2 * option + 1) / 8;
    rig.click(1, x, duty.rect.y + 60);
    assert(rig.host.values[P::kDuty] == option);
  }
  rig.click(3, duty.rect.x + 30, duty.rect.y + 60);
  assert(rig.host.values[P::kDuty] == P::kSpecs[P::kDuty].def);
  // The label row above the buttons is not a button.
  rig.host.reset_counts();
  rig.click(1, duty.rect.x + 30, duty.rect.y + 10);
  assert(rig.host.begins == 0);
  assert(rig.host.idle());
}

void test_menus() {
  Rig rig;
  const PlacedControl wave = rig.find(P::kWaveform);
  rig.click(1, wave.rect.x + 40, wave.rect.y + 60);
  assert(rig.editor.menu_open() && rig.editor.menu_param() == P::kWaveform);
  assert(rig.host.begins == 0 && "opening a menu edits nothing");
  rig.settle();
  // Everything in the menu is drawable inside the editor.
  RecordingCanvas canvas;
  rig.editor.draw(canvas);
  bool listed = false;
  for (const std::string& s : canvas.strings) listed = listed || s == P::kWaveNames[40];
  assert(listed);
  // Clicking outside closes it without an edit, and the click is not passed through.
  const double wave_before = rig.host.values[P::kWaveform];
  const PlacedControl vibrato = rig.find(P::kVibratoDepth);  // far right, clear of the menu
  rig.click(1, vibrato.rect.x + vibrato.rect.w / 2, vibrato.rect.y + 60);
  assert(!rig.editor.menu_open() && rig.host.values[P::kWaveform] == wave_before && rig.host.begins == 0);
  // Menu items are laid out in columns of menu_rows; item 17 is row 1 of column 1.
  rig.click(1, wave.rect.x + 40, wave.rect.y + 60);
  // Reconstruct the menu origin the same way the editor does.
  const int count = static_cast<int>(P::kSpecs[P::kWaveform].max) + 1;
  const int columns = (count + menu_rows - 1) / menu_rows;
  Rect menu{wave.rect.x, wave.rect.bottom() + 4, columns * menu_column_width + 16, std::min(count, menu_rows) * menu_item_height + 16};
  menu.x = std::clamp(menu.x, margin, width - margin - menu.w);
  if (menu.bottom() > content_bottom) menu.y = std::max(header_rect.bottom(), wave.rect.y - menu.h - 4);
  if (menu.bottom() > height - 4) menu.y = std::max(4, height - 4 - menu.h);
  assert(menu.x >= 0 && menu.right() <= width && menu.y >= 0 && menu.bottom() <= height);
  const int item = 17;
  rig.click(1, menu.x + 8 + (item / menu_rows) * menu_column_width + 30, menu.y + 8 + (item % menu_rows) * menu_item_height + 10);
  assert(!rig.editor.menu_open());
  assert(rig.host.values[P::kWaveform] == item && rig.host.idle() && rig.host.begins == 1);

  // The header preset menu works from any page, and the wheel steps it.
  rig.editor.set_page(4); rig.settle();
  rig.click(1, preset_rect.x + 50, preset_rect.y + 20);
  assert(rig.editor.menu_param() == P::kPreset);
  rig.click(1, 2, 2);
  assert(!rig.editor.menu_open());
  const double preset = rig.host.values[P::kPreset];
  rig.wheel(1, preset_rect.x + 50, preset_rect.y + 20);
  assert(rig.host.values[P::kPreset] == preset + 1 && rig.host.idle());
  // Switching pages closes an open menu.
  rig.click(1, preset_rect.x + 50, preset_rect.y + 20);
  assert(rig.editor.menu_open());
  rig.editor.set_page(0);
  assert(!rig.editor.menu_open());
}

// --- step lanes -----------------------------------------------------------------------------------

void test_lane_painting() {
  Rig rig;
  rig.go_to(P::kSequence1);
  const PlacedControl lane = rig.find(P::kSequence1);
  const LaneGeometry g = lane_geometry(lane.rect);
  const int step_w = g.step_width(8);
  const auto x_of = [&](int step) { return g.bars.x + step * step_w + step_w / 2; };
  // Paint from step 0 at the top to step 7 at the bottom, visiting every step.
  rig.down(1, x_of(0), g.bars.y + 1);
  assert(rig.host.values[P::kSequence1] == 24);
  for (int step = 1; step < 8; ++step) rig.move(x_of(step), g.bars.bottom() - 1);
  rig.up(1, x_of(7), g.bars.bottom() - 1);
  for (int step = 1; step < 8; ++step) assert(rig.host.values[static_cast<size_t>(P::kSequence1 + step)] == -24);
  // One gesture per touched step, all closed.
  assert(rig.host.idle() && rig.host.begins == 8 && rig.host.ends == 8);

  // Right-drag draws a straight line between the press and the pointer.
  rig.host.reset_counts();
  rig.down(3, x_of(0), g.bars.bottom() - 1);             // -24 at step 0
  rig.move(x_of(4), g.bars.y + 1);                      // +24 at step 4
  rig.up(3, x_of(4), g.bars.y + 1);
  for (int step = 0; step <= 4; ++step)
    assert(rig.host.values[static_cast<size_t>(P::kSequence1 + step)] == std::round(-24.0 + 48.0 * step / 4.0));
  assert(rig.host.values[P::kSequence1 + 5] == -24 && "steps beyond the line are untouched");
  assert(rig.host.idle());

  // A right-click without movement resets that one step.
  rig.click(3, x_of(2), g.bars.y + 10);
  assert(rig.host.values[P::kSequence1 + 2] == P::kSpecs[P::kSequence1 + 2].def && rig.host.idle());

  // The ruler sets the length, including by dragging along it.
  rig.click(1, x_of(5), g.ruler.y + 5);
  assert(rig.host.values[P::kSequenceLength] == 6);
  rig.down(1, x_of(1), g.ruler.y + 5);
  rig.move(x_of(2), g.ruler.y + 5);
  rig.move(x_of(7), g.ruler.y + 40);  // leaving the ruler vertically keeps the drag on it
  rig.up(1, x_of(7), g.ruler.y + 40);
  assert(rig.host.values[P::kSequenceLength] == 8 && rig.host.idle());
  for (int step = 0; step < 8; ++step) assert(rig.host.values[static_cast<size_t>(P::kSequence1 + step)] >= -24);
}

void test_duty_lane_levels() {
  Rig rig;
  rig.go_to(P::kDutyStep1);
  const PlacedControl lane = rig.find(P::kDutyStep1);
  const LaneGeometry g = lane_geometry(lane.rect);
  const int step_w = g.step_width(8);
  // Each duty owns a quarter of the height.
  for (int level = 0; level < 4; ++level) {
    const int y = g.bars.bottom() - 1 - level * g.bars.h / 4 - g.bars.h / 8;
    rig.click(1, g.bars.x + 3 * step_w + step_w / 2, y);
    assert(rig.host.values[P::kDutyStep4] == level);
  }
  // Painting outside the bars clamps rather than wrapping.
  rig.down(1, g.bars.x + step_w / 2, g.bars.y - 30);
  rig.move(g.bars.x - 100, g.bars.bottom() + 300);
  rig.up(1, g.bars.x - 100, g.bars.bottom() + 300);
  assert(rig.host.values[P::kDutyStep1] == 0 && rig.host.idle());
}

// --- robustness -----------------------------------------------------------------------------------

void test_lost_release_and_leave() {
  Rig rig;
  const PlacedControl attack = rig.find(P::kAttackMs);
  const PlacedControl release = rig.find(P::kReleaseMs);
  rig.down(1, attack.rect.x + 30, attack.rect.y + 60);
  // Leaving the window mid-drag keeps the drag (the platform captures the pointer).
  rig.editor.pointer(PointerAction::Leave, 0, 0, 0, 0, rig.now);
  assert(rig.editor.dragging() == P::kAttackMs);
  rig.move(attack.rect.x + 30, attack.rect.y - 200);
  // The release was lost; the next press must close the first gesture before opening another.
  rig.now += 1000.0;
  rig.down(1, release.rect.x + 30, release.rect.y + 60);
  assert(!rig.host.open.count(P::kAttackMs) && rig.host.open.count(P::kReleaseMs));
  rig.up(1, release.rect.x + 30, release.rect.y + 60);
  assert(rig.host.idle());
  // A release with no press, and releases of other buttons mid-drag, are harmless.
  rig.up(1, 10, 10);
  rig.down(1, attack.rect.x + 30, attack.rect.y + 60);
  rig.up(2, attack.rect.x + 30, attack.rect.y + 60);
  assert(rig.editor.dragging() == P::kAttackMs);
  rig.up(1, attack.rect.x + 30, attack.rect.y + 60);
  assert(rig.host.idle());
}

void test_strip_clicks_reach_host() {
  Rig rig;
  rig.editor.set_page(4); rig.settle();
  const PageLayout lay = rig.editor.layout();
  assert(lay.strip.h > 0);
  rig.click(3, lay.strip.x + 40, lay.strip.y + 40);
  assert(rig.host.strip_clicks.size() == 1 && rig.host.strip_clicks[0] == 43);
  // Pages without artwork have no strip to click.
  rig.editor.set_page(1); rig.settle();
  assert(rig.editor.layout().strip.h == 0);
}

// Every page, open, collapsed, mid-transition, with a menu, hovered: drawing stays in bounds and
// shows each visible control's label.
void test_drawing() {
  Rig rig;
  for (int page = 0; page < rig.editor.page_count(); ++page) {
    rig.editor.set_page(page);
    for (int frame = 0; frame < 3; ++frame) { RecordingCanvas c; rig.editor.draw(c); rig.editor.tick(rig.now += 16.0); }
    rig.settle();
    RecordingCanvas canvas;
    rig.editor.draw(canvas);
    assert(canvas.texts > 10 && canvas.rects > 10);
    const PageLayout lay = rig.editor.layout();
    for (const PlacedControl& placed : lay.controls) {
      const ControlSpec& spec = rig.editor.page_spec(page).sections[static_cast<size_t>(placed.section)].controls[static_cast<size_t>(placed.control)];
      if (spec.widget == Widget::Lane) continue;
      const char* label = rig.host.short_label(spec.param);
      bool shown = false;
      for (const std::string& s : canvas.strings) shown = shown || s == label || (s.size() > 3 && s.compare(s.size() - 3, 3, "...") == 0 && std::string(label).rfind(s.substr(0, s.size() - 3), 0) == 0);
      if (!shown) { std::fprintf(stderr, "label '%s' not drawn on page %d\n", label, page); assert(false); }
    }
    // Hover every control and redraw; the footer names it.
    for (const PlacedControl& placed : lay.controls) {
      rig.move(placed.rect.x + placed.rect.w / 2, placed.rect.y + 60);
      RecordingCanvas hovered;
      rig.editor.draw(hovered);
    }
    for (size_t s = 0; s < lay.sections.size(); ++s) rig.editor.set_section_open(page, static_cast<int>(s), false);
    for (int frame = 0; frame < 5; ++frame) { RecordingCanvas c; rig.editor.draw(c); rig.editor.tick(rig.now += 16.0); }
    rig.settle();
    RecordingCanvas collapsed;
    rig.editor.draw(collapsed);
    assert(rig.editor.layout().controls.empty());
  }
  // Extreme values draw too.
  for (int id = 0; id < kParams; ++id) rig.host.values[static_cast<size_t>(id)] = P::kSpecs[static_cast<size_t>(id)].max;
  for (int page = 0; page < rig.editor.page_count(); ++page) {
    rig.editor.set_page(page);
    for (size_t s = 0; s < rig.editor.page_spec(page).sections.size(); ++s) rig.editor.set_section_open(page, static_cast<int>(s), true);
    rig.settle();
    RecordingCanvas canvas;
    rig.editor.draw(canvas);
  }
}

// Cell labels are drawn about cell_target_width wide; short names exist to fit there.
void test_short_names_fit() {
  for (int id = 0; id < kParams; ++id)
    if (const char* name = yanes_short_name(id)) assert(std::strlen(name) <= 14);
}

}  // namespace

int main() {
  test_scaling();
  test_every_parameter_is_placed_once();
  test_widgets_match_parameters();
  test_pages_fit_open();
  test_tabs_switch_and_settle();
  test_sections_collapse_and_reflow();
  test_knob_drag();
  test_stepped_knob_snaps();
  test_knob_resets();
  test_wheel();
  test_toggle_and_segments();
  test_menus();
  test_lane_painting();
  test_duty_lane_levels();
  test_lost_release_and_leave();
  test_strip_clicks_reach_host();
  test_drawing();
  test_short_names_fit();
  std::printf("ui_tests: all checks passed\n");
}
