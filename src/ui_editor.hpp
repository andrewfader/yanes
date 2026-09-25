// The platform-neutral editor: page and card layout, widgets, pointer handling, and animation.
//
// It knows nothing about the plug-in. Parameters, formatting, and edits go through EditorHost,
// and all drawing goes through Canvas, so the whole editor can be driven by the tests with a
// fake host and a recording canvas. The plug-in supplies the host (ui_frontend.hpp) and each
// platform supplies the canvas.
//
// Interaction conventions follow the common ground of mainstream synth editors:
//   knobs      vertical drag (250 units for the full range), Shift for fine, double-click,
//              Ctrl-click or right-click to reset, wheel to nudge
//   choices    segmented buttons for short lists, a pop-up menu for long ones
//   toggles    click
//   step lanes paint by dragging, right-drag draws a line, click the ruler to set the length
// Every edit is wrapped in a host gesture: a drag is one gesture per parameter it touches.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ui_canvas.hpp"
#include "ui_layout.hpp"

namespace yanes::ui {

inline double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// --- colours -----------------------------------------------------------------------------------

namespace theme {
constexpr uint32_t bg = 0x0b1119, panel = 0x111b27, panel_hi = 0x192739, raised = 0x1f3047;
constexpr uint32_t border = 0x26384b, text = 0xe8eef6, soft = 0xc3cfdc, muted = 0x8495a8;
constexpr uint32_t faint = 0x4d5d70, cyan = 0x5bd8ff, green = 0x42d392, amber = 0xffc857;
constexpr uint32_t red = 0xf0647d, violet = 0xb18cff, pink = 0xff7ac8;
}  // namespace theme

// Linear blend from a to b; t = 0 gives a.
constexpr uint32_t mix(uint32_t a, uint32_t b, double t) {
  const double k = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const double ca = static_cast<double>((a >> shift) & 255U), cb = static_cast<double>((b >> shift) & 255U);
    out |= static_cast<uint32_t>(ca + (cb - ca) * k + 0.5) << shift;
  }
  return out;
}

// Wraps a Canvas with a fade toward the background and an offset, which is how page changes
// cross-fade without any backend support for transparency.
class Painter {
 public:
  explicit Painter(Canvas& canvas) : canvas_(canvas) {}
  double fade = 1.0;
  int dx = 0, dy = 0;

  uint32_t color(uint32_t rgb) const { return fade >= 1.0 ? rgb : mix(theme::bg, rgb, fade); }
  void rect(int x, int y, int w, int h, uint32_t rgb) {
    if (w > 0 && h > 0) canvas_.fill_rect(x + dx, y + dy, w, h, color(rgb));
  }
  void rect(const Rect& r, uint32_t rgb) { rect(r.x, r.y, r.w, r.h, rgb); }
  // Rounded corners from four quarter discs; enough at the radii the editor uses.
  void round_rect(const Rect& r, int radius, uint32_t rgb) {
    radius = std::clamp(radius, 0, std::min(r.w, r.h) / 2);
    if (radius <= 1) { rect(r, rgb); return; }
    rect(r.x + radius, r.y, r.w - 2 * radius, r.h, rgb);
    rect(r.x, r.y + radius, radius, r.h - 2 * radius, rgb);
    rect(r.right() - radius, r.y + radius, radius, r.h - 2 * radius, rgb);
    const int d = radius * 2;
    circle(r.x, r.y, d, rgb); circle(r.right() - d, r.y, d, rgb);
    circle(r.x, r.bottom() - d, d, rgb); circle(r.right() - d, r.bottom() - d, d, rgb);
  }
  void circle(int x, int y, int diameter, uint32_t rgb) { canvas_.fill_circle(x + dx, y + dy, diameter, color(rgb)); }
  void line(int x1, int y1, int x2, int y2, uint32_t rgb, int thickness = 1) {
    canvas_.draw_line(x1 + dx, y1 + dy, x2 + dx, y2 + dy, color(rgb), thickness);
  }
  void polyline(std::vector<Point> points, uint32_t rgb, int thickness = 1) {
    for (auto& point : points) { point.x += dx; point.y += dy; }
    canvas_.draw_polyline(points.data(), static_cast<int>(points.size()), color(rgb), thickness);
  }
  void text(int x, int y, const char* s, uint32_t rgb, int max_width = 0, TextSize size = TextSize::Normal) {
    canvas_.draw_text(x + dx, y + dy, s, color(rgb), max_width, size);
  }
  void text_centered(int cx, int y, const char* s, uint32_t rgb, int max_width, TextSize size) {
    const std::string fitted = canvas_.fit(s, max_width, size);
    const int w = canvas_.text_width(fitted.c_str(), size);
    canvas_.draw_text(cx - w / 2 + dx, y + dy, fitted.c_str(), color(rgb), 0, size);
  }
  void text_right(int right, int y, const char* s, uint32_t rgb, int max_width, TextSize size) {
    const std::string fitted = canvas_.fit(s, max_width, size);
    canvas_.draw_text(right - canvas_.text_width(fitted.c_str(), size) + dx, y + dy, fitted.c_str(), color(rgb), 0, size);
  }
  int text_width(const char* s, TextSize size) { return canvas_.text_width(s, size); }

 private:
  Canvas& canvas_;
};

// --- what the editor edits ---------------------------------------------------------------------

struct ParamInfo {
  const char* name;
  double min, max, def;
  bool stepped;
};

class EditorHost {
 public:
  virtual ~EditorHost() = default;
  virtual int param_count() const = 0;
  virtual ParamInfo info(int id) const = 0;
  virtual double value(int id) const = 0;
  // A gesture brackets one or more edits of one parameter, as CLAP expects.
  virtual void begin_edit(int id) = 0;
  virtual void edit(int id, double value) = 0;
  virtual void end_edit(int id) = 0;
  virtual void format(int id, double value, char* out, size_t capacity) const = 0;
  // Display name, which may depend on the selected sound source; short_label fits under a knob.
  virtual const char* label(int id) const { return info(id).name; }
  virtual const char* short_label(int id) const { return label(id); }
  virtual const char* help(int id) const { (void)id; return ""; }
  // False dims the control: it exists, but does nothing for the current sound source.
  virtual bool relevant(int id) const { (void)id; return true; }
  // Page artwork above the cards (scope, mixer tiles, ...). Returns true when a click was used.
  virtual void draw_strip(int page, Painter& painter, const Rect& area) { (void)page; (void)painter; (void)area; }
  virtual bool strip_pointer(int page, int button, int x, int y, const Rect& area) {
    (void)page; (void)button; (void)x; (void)y; (void)area; return false;
  }
  virtual const char* strip_help(int page) const { (void)page; return nullptr; }
  virtual void draw_meters(Painter& painter, const Rect& area) { (void)painter; (void)area; }
  // Editor window size in percent of the design size, or 0 when the host cannot resize it (the
  // SIZE button is then hidden). request_size asks for a new size; it may take effect later.
  virtual int size_percent() const { return 0; }
  virtual void request_size(int percent) { (void)percent; }
};

// The next SIZE step from `current` in `direction` (-1 smaller, +1 larger), clamped at the ends.
inline int next_size_step(int current, int direction) {
  constexpr int count = static_cast<int>(sizeof(size_steps) / sizeof(size_steps[0]));
  if (direction < 0) {
    for (int i = count - 1; i >= 0; --i) if (size_steps[i] < current) return size_steps[i];
    return size_steps[0];
  }
  for (int i = 0; i < count; ++i) if (size_steps[i] > current) return size_steps[i];
  return size_steps[count - 1];
}

// --- page description --------------------------------------------------------------------------

enum class Widget : uint8_t { Knob, Toggle, Segments, Menu, Lane };

struct ControlSpec {
  Widget widget;
  int param;        // for a lane, the first step
  int span = 1;     // cells; clamped to the row
  int steps = 0;    // lane: number of step parameters
  int length = -1;  // lane: the parameter holding how many steps play
  const char* caption = nullptr;  // lane: title above the steps
};

struct SectionSpec {
  const char* title;
  int column;
  std::vector<ControlSpec> controls;
};

struct PageSpec {
  const char* tab;
  const char* help;
  uint32_t accent;
  int columns;
  int strip_height;  // 0 for no artwork strip
  std::vector<SectionSpec> sections;
};

// --- layout ------------------------------------------------------------------------------------

struct PlacedControl {
  int section, control;
  Rect rect;
};

struct PlacedSection {
  Rect card;    // current (animated) extent
  Rect header;
  int body_height;  // fully open body height
  int visible_body; // body height shown at the current openness
};

struct PageLayout {
  std::vector<PlacedSection> sections;
  std::vector<PlacedControl> controls;  // only those fully inside their card's visible body
  Rect strip;
  int content_top;
};

inline int column_width(int columns) { return (width - 2 * margin - column_gap * (columns - 1)) / columns; }
inline int cells_per_row(int columns) { return std::max(1, (column_width(columns) - 2 * card_padding) / cell_target_width); }

// Lays out one page for the given per-section openness (0 closed .. 1 open).
inline PageLayout layout_page(const PageSpec& page, const std::vector<double>& openness) {
  PageLayout out;
  out.strip = Rect{margin, strip_y, width - 2 * margin, page.strip_height};
  out.content_top = page.strip_height > 0 ? strip_y + page.strip_height + card_gap : strip_y;
  const int col_w = column_width(page.columns);
  const int per_row = cells_per_row(page.columns);
  const int cell_w = (col_w - 2 * card_padding) / per_row;
  std::vector<int> column_y(static_cast<size_t>(page.columns), out.content_top);
  out.sections.resize(page.sections.size());
  for (size_t s = 0; s < page.sections.size(); ++s) {
    const SectionSpec& spec = page.sections[s];
    const int column = std::clamp(spec.column, 0, page.columns - 1);
    const int x = margin + column * (col_w + column_gap);
    const int y = column_y[static_cast<size_t>(column)];
    // Flow controls into rows, measuring the body as if fully open.
    struct Cell { size_t index; int row_y, cell, span; bool lane; };
    std::vector<Cell> cells;
    int row_y = 0, cursor = 0, row_h = 0;
    for (size_t c = 0; c < spec.controls.size(); ++c) {
      const ControlSpec& control = spec.controls[c];
      const bool lane = control.widget == Widget::Lane;
      const int span = lane ? per_row : std::clamp(control.span, 1, per_row);
      if (cursor > 0 && cursor + span > per_row) { row_y += row_h; cursor = 0; row_h = 0; }
      cells.push_back({c, row_y, cursor, span, lane});
      row_h = std::max(row_h, lane ? lane_height : cell_height);
      cursor += span;
    }
    const int body = cells.empty() ? 0 : row_y + row_h + card_padding;
    const double open = openness.size() > s ? std::clamp(openness[s], 0.0, 1.0) : 1.0;
    const int visible = static_cast<int>(std::lround(body * open));
    PlacedSection& placed = out.sections[s];
    placed.card = Rect{x, y, col_w, card_header + visible};
    placed.header = Rect{x, y, col_w, card_header};
    placed.body_height = body;
    placed.visible_body = visible;
    for (const Cell& cell : cells) {
      const Rect r{x + card_padding + cell.cell * cell_w, y + card_header + cell.row_y, cell.span * cell_w,
                   cell.lane ? lane_height : cell_height};
      if (r.bottom() <= y + card_header + visible)
        out.controls.push_back({static_cast<int>(s), static_cast<int>(cell.index), r});
    }
    column_y[static_cast<size_t>(column)] = y + placed.card.h + card_gap;
  }
  return out;
}

// Lane geometry shared by drawing and hit-testing.
struct LaneGeometry {
  Rect bars, ruler;
  int gutter;
  int step_width(int steps) const { return steps > 0 ? bars.w / steps : bars.w; }
};
inline LaneGeometry lane_geometry(const Rect& r) {
  constexpr int gutter = 78;
  LaneGeometry g;
  g.gutter = gutter;
  g.bars = Rect{r.x + gutter, r.y + 34, r.w - gutter - 6, r.h - 34 - 40};
  g.ruler = Rect{g.bars.x, g.bars.bottom() + 6, g.bars.w, 28};
  return g;
}

// Menus lay their options out in columns of up to this many rows.
constexpr int menu_rows = 16;
constexpr int menu_item_height = 34;
constexpr int menu_column_width = 290;

// --- the editor --------------------------------------------------------------------------------

enum Modifier : unsigned { kShift = 1U, kControl = 2U, kAlt = 4U };
enum class PointerAction { Move, Down, Up, Leave };

class Editor {
 public:
  static constexpr double drag_range = 250.0;   // design units for a full sweep
  static constexpr double fine_factor = 0.1;
  static constexpr double double_click_ms = 350.0;
  static constexpr double transition_ms = 150.0;
  static constexpr double wheel_fraction = 0.02;

  Editor(EditorHost& host, std::vector<PageSpec> pages, ControlSpec header_control)
      : host_(host), pages_(std::move(pages)), header_control_(header_control) {
    for (const PageSpec& page : pages_) {
      openness_.emplace_back(page.sections.size(), 1.0);
      open_target_.emplace_back(page.sections.size(), true);
    }
    tab_position_ = 0.0;
  }

  // --- state queries (also used by the tests) ---
  int page() const { return page_; }
  int page_count() const { return static_cast<int>(pages_.size()); }
  const PageSpec& page_spec(int index) const { return pages_[static_cast<size_t>(index)]; }
  bool section_open(int page, int section) const { return open_target_[static_cast<size_t>(page)][static_cast<size_t>(section)]; }
  double section_openness(int page, int section) const { return openness_[static_cast<size_t>(page)][static_cast<size_t>(section)]; }
  bool menu_open() const { return menu_.param >= 0; }
  int menu_param() const { return menu_.param; }
  int dragging() const { return drag_.param; }
  bool animating() const { return animating_; }
  PageLayout layout() const { return layout_page(pages_[static_cast<size_t>(page_)], openness_[static_cast<size_t>(page_)]); }
  Rect header_control_rect() const { return preset_rect; }

  void set_page(int index) {
    if (index < 0 || index >= page_count() || index == page_) return;
    close_menu();
    hover_ = Hit{};
    page_ = index;
    page_fade_ = 0.0;
    start_animation();
  }
  void set_section_open(int page, int section, bool open) {
    open_target_[static_cast<size_t>(page)][static_cast<size_t>(section)] = open;
    start_animation();
  }

  // Advances animations to `now`. Returns true while anything is still moving, so the caller
  // repaints only then.
  bool tick(double now) {
    // An animation that starts after an idle stretch begins from one frame, not from the gap.
    const double dt = last_tick_ < 0.0 ? 16.0 : std::clamp(now - last_tick_, 0.0, 100.0);
    last_tick_ = now;
    bool moving = false;
    const auto approach = [&](double& value, double target) {
      const double step = dt / transition_ms;
      if (std::abs(value - target) <= step) value = target;
      else { value += value < target ? step : -step; moving = true; }
    };
    approach(page_fade_, 1.0);
    // The tab indicator slides with an ease-out rather than linearly.
    const double tab_target = static_cast<double>(page_);
    const double gap = tab_target - tab_position_;
    if (std::abs(gap) < 0.002) tab_position_ = tab_target;
    else { tab_position_ += gap * std::min(1.0, dt / 45.0); moving = true; }
    for (size_t p = 0; p < openness_.size(); ++p)
      for (size_t s = 0; s < openness_[p].size(); ++s) approach(openness_[p][s], open_target_[p][s] ? 1.0 : 0.0);
    if (menu_.param >= 0) approach(menu_.fade, 1.0);
    animating_ = moving;
    return moving;
  }

  // --- input ---

  void pointer(PointerAction action, int button, int x, int y, unsigned modifiers, double now) {
    if (action == PointerAction::Leave) { hover_ = Hit{}; menu_.hover = -1; return; }
    if (action == PointerAction::Move) { move(x, y, modifiers); return; }
    if (action == PointerAction::Up) { release(button); return; }
    if (button == 4 || button == 5) { wheel(x, y, button == 4 ? 1 : -1, modifiers); return; }
    press(button, x, y, modifiers, now);
  }

  // --- drawing ---

  void draw(Canvas& canvas) {
    using namespace theme;
    canvas.clear(bg);
    Painter chrome(canvas);
    draw_header(chrome);
    draw_tabs(chrome);
    const PageSpec& page = pages_[static_cast<size_t>(page_)];
    const PageLayout lay = layout();
    const double eased = ease(page_fade_);
    Painter content(canvas);
    content.fade = eased;
    content.dy = static_cast<int>(std::lround((1.0 - eased) * 12.0));
    if (page.strip_height > 0) {
      content.round_rect(lay.strip, 10, panel);
      host_.draw_strip(page_, content, lay.strip);
    }
    for (size_t s = 0; s < page.sections.size(); ++s) draw_section(content, page, lay, static_cast<int>(s));
    draw_footer(chrome);
    if (menu_.param >= 0) draw_menu(canvas);
  }

 private:
  enum class Target : uint8_t { Empty, Tab, Section, Control, Header, Strip, Size };
  struct Hit {
    Target target = Target::Empty;
    int index = -1;    // tab, section
    int control = -1;  // control within section
    int param = -1;
    int sub = -1;      // segment, lane step
    bool ruler = false;
    Rect rect{};
    bool operator==(const Hit& o) const {
      return target == o.target && index == o.index && control == o.control && param == o.param;
    }
  };
  struct Drag {
    int param = -1;
    Widget widget = Widget::Knob;
    double value = 0.0;   // unrounded accumulator for knobs
    int last_y = 0;
    ControlSpec lane{};
    Rect rect{};
    bool line = false;    // right-drag in a lane draws a straight line
    bool moved = false;
    int anchor_step = 0;
    double anchor_value = 0.0;
    std::vector<int> touched;
  };
  struct Menu {
    int param = -1;
    Rect rect{};
    int hover = -1;
    double fade = 0.0;
  };

  void start_animation() { animating_ = true; last_tick_ = -1.0; }

  static double ease(double t) { t = std::clamp(t, 0.0, 1.0); return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t); }

  const ControlSpec& control_spec(int section, int control) const {
    return pages_[static_cast<size_t>(page_)].sections[static_cast<size_t>(section)].controls[static_cast<size_t>(control)];
  }

  int option_count(int param) const {
    const ParamInfo info = host_.info(param);
    return static_cast<int>(std::lround(info.max - info.min)) + 1;
  }

  // --- hit testing ---

  Hit hit_test(int x, int y) const {
    Hit hit;
    if (preset_rect.contains(x, y)) {
      hit.target = Target::Header; hit.param = header_control_.param; hit.rect = preset_rect;
      return hit;
    }
    if (host_.size_percent() > 0 && size_rect.contains(x, y)) {
      hit.target = Target::Size; hit.rect = size_rect;
      return hit;
    }
    if (const int tab = tab_at(x, y); tab >= 0 && tab < page_count()) {
      hit.target = Target::Tab; hit.index = tab; hit.rect = tab_rect(tab);
      return hit;
    }
    const PageLayout lay = layout();
    if (lay.strip.h > 0 && lay.strip.contains(x, y)) { hit.target = Target::Strip; hit.rect = lay.strip; return hit; }
    for (size_t s = 0; s < lay.sections.size(); ++s)
      if (lay.sections[s].header.contains(x, y)) {
        hit.target = Target::Section; hit.index = static_cast<int>(s); hit.rect = lay.sections[s].header;
        return hit;
      }
    for (const PlacedControl& placed : lay.controls) {
      if (!placed.rect.contains(x, y)) continue;
      const ControlSpec& spec = control_spec(placed.section, placed.control);
      hit.target = Target::Control; hit.index = placed.section; hit.control = placed.control;
      hit.param = spec.param; hit.rect = placed.rect;
      if (spec.widget == Widget::Segments) {
        const Rect row = segments_rect(placed.rect);
        const int count = option_count(spec.param);
        if (!row.contains(x, y)) { hit.sub = -1; return hit; }
        hit.sub = std::clamp((x - row.x) * count / std::max(1, row.w), 0, count - 1);
      } else if (spec.widget == Widget::Lane) {
        const LaneGeometry g = lane_geometry(placed.rect);
        const int step_w = g.step_width(spec.steps);
        if (g.bars.contains(x, y) || g.ruler.contains(x, y)) {
          hit.sub = std::clamp((x - g.bars.x) / std::max(1, step_w), 0, spec.steps - 1);
          hit.ruler = g.ruler.contains(x, y);
          hit.param = hit.ruler ? spec.length : spec.param + hit.sub;
        } else {
          hit.param = -1;
        }
      }
      return hit;
    }
    return hit;
  }

  static Rect segments_rect(const Rect& cell) { return Rect{cell.x + 8, cell.y + 44, cell.w - 16, 42}; }
  static Rect menu_box_rect(const Rect& cell) { return Rect{cell.x + 8, cell.y + 44, cell.w - 16, 42}; }

  // --- gestures ---

  void set_now(int param, double value) {
    host_.begin_edit(param);
    host_.edit(param, value);
    host_.end_edit(param);
  }
  void reset(int param) { if (param >= 0) set_now(param, host_.info(param).def); }
  void touch(int param, double value) {
    if (std::find(drag_.touched.begin(), drag_.touched.end(), param) == drag_.touched.end()) {
      drag_.touched.push_back(param);
      host_.begin_edit(param);
    }
    host_.edit(param, value);
  }
  void end_drag() {
    for (const int param : drag_.touched) host_.end_edit(param);
    drag_ = Drag{};
  }

  double lane_value_at(const ControlSpec& lane, const Rect& rect, int y) const {
    const LaneGeometry g = lane_geometry(rect);
    const ParamInfo info = host_.info(lane.param);
    const double norm = 1.0 - std::clamp(static_cast<double>(y - g.bars.y) / std::max(1, g.bars.h), 0.0, 1.0);
    if (info.min >= 0.0 && info.max - info.min <= 8.0) {
      // Few discrete levels (duty): each owns an equal horizontal band.
      const int levels = static_cast<int>(std::lround(info.max - info.min)) + 1;
      return info.min + std::clamp(static_cast<int>(norm * levels), 0, levels - 1);
    }
    return std::round(info.min + norm * (info.max - info.min));
  }

  void paint_lane(int x, int y) {
    const ControlSpec& lane = drag_.lane;
    const LaneGeometry g = lane_geometry(drag_.rect);
    const int step_w = g.step_width(lane.steps);
    const int step = std::clamp((x - g.bars.x) / std::max(1, step_w), 0, lane.steps - 1);
    const double value = lane_value_at(lane, drag_.rect, y);
    if (!drag_.line) { touch(lane.param + step, value); return; }
    // Straight line from the anchor step to here, like a tracker macro editor's right-drag.
    const int from = std::min(drag_.anchor_step, step), to = std::max(drag_.anchor_step, step);
    for (int s = from; s <= to; ++s) {
      const double t = step == drag_.anchor_step ? 1.0 : static_cast<double>(s - drag_.anchor_step) / (step - drag_.anchor_step);
      touch(lane.param + s, std::round(drag_.anchor_value + (value - drag_.anchor_value) * t));
    }
  }

  void press(int button, int x, int y, unsigned modifiers, double now) {
    if (drag_.param >= 0) end_drag();  // a lost release must never leave a gesture open
    if (menu_.param >= 0) { menu_press(button, x, y); return; }
    const Hit hit = hit_test(x, y);
    const bool double_click = button == 1 && hit.target == Target::Control && hit == last_click_ &&
                              now - last_click_time_ <= double_click_ms;
    last_click_ = button == 1 ? hit : Hit{};
    last_click_time_ = now;
    switch (hit.target) {
      case Target::Tab: if (button == 1) set_page(hit.index); return;
      case Target::Section:
        if (button == 1) set_section_open(page_, hit.index, !section_open(page_, hit.index));
        return;  // hover stays on the header, which does not move
      case Target::Header:
        if (button == 1) open_menu(hit.param, hit.rect);
        else if (button == 3) reset(hit.param);
        return;
      case Target::Size: {
        // Click steps down and wraps from the smallest back to the largest; right-click restores 100%.
        const int current = host_.size_percent();
        if (button == 3) host_.request_size(100);
        else if (button == 1) host_.request_size(current <= size_steps[0] ? next_size_step(1000, 1) : next_size_step(current, -1));
        return;
      }
      case Target::Strip: {
        const PageLayout lay = layout();
        host_.strip_pointer(page_, button, x, y, lay.strip);
        return;
      }
      case Target::Control: break;
      default: return;
    }
    const ControlSpec& spec = control_spec(hit.index, hit.control);
    const bool reset_click = button == 3 || (button == 1 && ((modifiers & kControl) || double_click));
    switch (spec.widget) {
      case Widget::Knob:
        if (reset_click) { reset(spec.param); last_click_ = Hit{}; return; }
        if (button != 1) return;
        drag_.param = spec.param; drag_.widget = Widget::Knob;
        drag_.value = host_.value(spec.param); drag_.last_y = y;
        touch(spec.param, drag_.value);
        return;
      case Widget::Toggle:
        if (button == 3) { reset(spec.param); return; }
        if (button == 1) {
          const ParamInfo info = host_.info(spec.param);
          set_now(spec.param, host_.value(spec.param) >= (info.min + info.max) * 0.5 ? info.min : info.max);
        }
        return;
      case Widget::Segments:
        if (button == 3) { reset(spec.param); return; }
        if (button == 1 && hit.sub >= 0) set_now(spec.param, host_.info(spec.param).min + hit.sub);
        return;
      case Widget::Menu:
        if (button == 3) { reset(spec.param); return; }
        if (button == 1) open_menu(spec.param, hit.rect);
        return;
      case Widget::Lane: {
        if (hit.param < 0) return;
        if (hit.ruler) {
          if (button == 3) { reset(spec.length); return; }
          if (button != 1) return;
          drag_.param = spec.length; drag_.widget = Widget::Lane; drag_.lane = spec; drag_.rect = hit.rect;
          touch(spec.length, host_.info(spec.length).min + hit.sub);
          drag_.moved = true;
          return;
        }
        if (button != 1 && button != 3) return;
        drag_.param = spec.param; drag_.widget = Widget::Lane; drag_.lane = spec; drag_.rect = hit.rect;
        drag_.line = button == 3;
        drag_.anchor_step = hit.sub;
        drag_.anchor_value = lane_value_at(spec, hit.rect, y);
        if (!drag_.line) paint_lane(x, y);
        return;
      }
    }
  }

  void move(int x, int y, unsigned modifiers) {
    if (menu_.param >= 0) { menu_.hover = menu_item_at(x, y); return; }
    if (drag_.param < 0) { hover_ = hit_test(x, y); return; }
    if (drag_.widget == Widget::Knob) {
      const ParamInfo info = host_.info(drag_.param);
      const double scale = (modifiers & kShift) ? fine_factor : 1.0;
      drag_.value = std::clamp(drag_.value + (drag_.last_y - y) / drag_range * (info.max - info.min) * scale,
                               info.min, info.max);
      drag_.last_y = y;
      host_.edit(drag_.param, info.stepped ? std::round(drag_.value) : drag_.value);
      return;
    }
    // Lanes: either the ruler (length) or painting steps.
    const LaneGeometry g = lane_geometry(drag_.rect);
    const int step_w = g.step_width(drag_.lane.steps);
    const int step = std::clamp((x - g.bars.x) / std::max(1, step_w), 0, drag_.lane.steps - 1);
    if (drag_.param == drag_.lane.length && drag_.moved) {
      touch(drag_.lane.length, host_.info(drag_.lane.length).min + step);
      return;
    }
    drag_.moved = true;
    paint_lane(x, y);
  }

  void release(int button) {
    if (drag_.param < 0) return;
    if (drag_.widget == Widget::Lane && drag_.line && !drag_.moved && button == 3) {
      // A right-click without a drag resets that one step.
      const int param = drag_.lane.param + drag_.anchor_step;
      drag_ = Drag{};
      reset(param);
      return;
    }
    if (button == 1 || button == 3) end_drag();
  }

  void wheel(int x, int y, int direction, unsigned modifiers) {
    if (menu_.param >= 0) return;
    const Hit hit = hit_test(x, y);
    if (hit.target == Target::Size) { host_.request_size(next_size_step(host_.size_percent(), direction)); return; }
    int param = hit.param;
    if (hit.target == Target::Header) param = header_control_.param;
    if (param < 0 || (hit.target != Target::Control && hit.target != Target::Header)) return;
    const ParamInfo info = host_.info(param);
    const double old = host_.value(param);
    double step = info.stepped ? 1.0 : (info.max - info.min) * wheel_fraction * ((modifiers & kShift) ? fine_factor : 1.0);
    if (hit.target == Target::Control) {
      const ControlSpec& spec = control_spec(hit.index, hit.control);
      if (spec.widget == Widget::Toggle) { set_now(param, direction > 0 ? info.max : info.min); return; }
    }
    set_now(param, std::clamp(old + direction * step, info.min, info.max));
  }

  // --- menu ---

  void open_menu(int param, const Rect& anchor) {
    const int count = option_count(param);
    const int columns = (count + menu_rows - 1) / menu_rows;
    const int rows = std::min(count, menu_rows);
    Rect r{anchor.x, anchor.bottom() + 4, columns * menu_column_width + 16, rows * menu_item_height + 16};
    r.x = std::clamp(r.x, margin, width - margin - r.w);
    if (r.bottom() > content_bottom) r.y = std::max(header_rect.bottom(), anchor.y - r.h - 4);
    if (r.bottom() > height - 4) r.y = std::max(4, height - 4 - r.h);
    menu_ = Menu{param, r, -1, 0.0};
    start_animation();
  }
  void close_menu() { menu_ = Menu{}; }

  Rect menu_item_rect(int index) const {
    const int column = index / menu_rows, row = index % menu_rows;
    return Rect{menu_.rect.x + 8 + column * menu_column_width, menu_.rect.y + 8 + row * menu_item_height,
                menu_column_width - 6, menu_item_height - 2};
  }
  int menu_item_at(int x, int y) const {
    if (menu_.param < 0 || !menu_.rect.contains(x, y)) return -1;
    const int count = option_count(menu_.param);
    for (int i = 0; i < count; ++i) if (menu_item_rect(i).contains(x, y)) return i;
    return -1;
  }
  void menu_press(int button, int x, int y) {
    const int item = menu_item_at(x, y);
    const int param = menu_.param;
    if (button == 4 || button == 5) return;
    close_menu();
    if (item >= 0 && button == 1) set_now(param, host_.info(param).min + item);
  }

  // --- drawing pieces ---

  void draw_header(Painter& p) {
    using namespace theme;
    p.text(margin, 46, "YANES", text, 140, TextSize::Large);
    p.text(margin + 132, 44, "RETRO CHIP WORKSTATION", muted, preset_rect.x - margin - 150, TextSize::Small);
    // Preset selector: always reachable, whatever the page.
    const bool hover = hover_.target == Target::Header || menu_.param == header_control_.param;
    p.round_rect(preset_rect, 8, hover ? raised : panel_hi);
    p.text(preset_rect.x + 16, preset_rect.y + 29, "PRESET", muted, 90, TextSize::Small);
    char value[96]{};
    host_.format(header_control_.param, host_.value(header_control_.param), value, sizeof(value));
    p.text(preset_rect.x + 104, preset_rect.y + 30, value, text, preset_rect.w - 150);
    draw_chevron(p, preset_rect.right() - 30, preset_rect.y + 20, hover ? cyan : muted);
    if (const int percent = host_.size_percent(); percent > 0) {
      p.round_rect(size_rect, 8, hover_.target == Target::Size ? raised : panel_hi);
      p.text(size_rect.x + 14, size_rect.y + 29, "SIZE", muted, 50, TextSize::Small);
      char label[16]{};
      std::snprintf(label, sizeof(label), "%d%%", percent);
      p.text_right(size_rect.right() - 16, size_rect.y + 30, label, text, 80, TextSize::Normal);
    }
    host_.draw_meters(p, meter_rect);
  }

  void draw_chevron(Painter& p, int x, int y, uint32_t rgb) {
    p.polyline({{x - 7, y - 3}, {x, y + 4}, {x + 7, y - 3}}, rgb, 3);
  }

  void draw_tabs(Painter& p) {
    using namespace theme;
    for (int i = 0; i < page_count(); ++i) {
      const Rect r = tab_rect(i);
      const bool active = i == page_, hover = hover_.target == Target::Tab && hover_.index == i;
      p.round_rect(r, 8, active ? panel_hi : (hover ? 0x152232 : panel));
      char label[48]{};
      std::snprintf(label, sizeof(label), "%02d  %s", i + 1, pages_[static_cast<size_t>(i)].tab);
      p.text(r.x + 18, r.y + 31, label, active ? text : (hover ? soft : muted), r.w - 30);
    }
    // One indicator that slides between tabs and takes the colour of the page it lands on.
    const Rect from = tab_rect(static_cast<int>(std::floor(tab_position_)));
    const double frac = tab_position_ - std::floor(tab_position_);
    const int x = from.x + static_cast<int>(std::lround(frac * tab_width));
    p.rect(x + 12, tab_y + tab_height - 5, tab_width - 32, 4, pages_[static_cast<size_t>(page_)].accent);
  }

  void draw_section(Painter& p, const PageSpec& page, const PageLayout& lay, int s) {
    using namespace theme;
    const PlacedSection& placed = lay.sections[static_cast<size_t>(s)];
    const SectionSpec& spec = page.sections[static_cast<size_t>(s)];
    bool any_relevant = false;
    for (const ControlSpec& c : spec.controls) any_relevant = any_relevant || host_.relevant(c.param);
    const bool hover = hover_.target == Target::Section && hover_.index == s;
    p.round_rect(placed.card, 10, panel);
    p.round_rect(placed.header, 10, hover ? raised : panel_hi);
    if (placed.visible_body > 0) p.rect(placed.header.x, placed.header.bottom() - 10, placed.header.w, 10, hover ? raised : panel_hi);
    // Disclosure triangle, rotating with the openness.
    const double open = section_openness(page_, s);
    const int cx = placed.header.x + 22, cy = placed.header.y + 21;
    const double angle = open * 1.5707963;
    const auto rot = [&](double px, double py) {
      return Point{cx + static_cast<int>(std::lround(px * std::cos(angle) - py * std::sin(angle))),
                   cy + static_cast<int>(std::lround(px * std::sin(angle) + py * std::cos(angle)))};
    };
    p.polyline({rot(-3, -7), rot(4, 0), rot(-3, 7)}, hover ? text : muted, 3);
    p.rect(placed.header.x, placed.header.y + 8, 3, card_header - 16, any_relevant ? page.accent : faint);
    p.text(placed.header.x + 42, placed.header.y + 29, spec.title, any_relevant ? text : muted, placed.header.w - 190);
    if (!any_relevant) p.text_right(placed.header.right() - 14, placed.header.y + 28, "inactive for this source", faint, 220, TextSize::Small);
    else if (open < 0.5) {
      char count[40]{};
      std::snprintf(count, sizeof(count), "%zu controls", spec.controls.size());
      p.text_right(placed.header.right() - 14, placed.header.y + 28, count, muted, 140, TextSize::Small);
    }
    for (const PlacedControl& control : lay.controls)
      if (control.section == s) draw_control(p, page, spec.controls[static_cast<size_t>(control.control)], control);
  }

  bool is_hovered(const PlacedControl& placed) const {
    return hover_.target == Target::Control && hover_.index == placed.section && hover_.control == placed.control;
  }

  void draw_control(Painter& p, const PageSpec& page, const ControlSpec& spec, const PlacedControl& placed) {
    using namespace theme;
    const bool relevant = host_.relevant(spec.param);
    const bool active = is_hovered(placed) || drag_.param == spec.param ||
                        (drag_.widget == Widget::Lane && drag_.lane.param == spec.param && drag_.param >= 0);
    const uint32_t accent = relevant ? page.accent : faint;
    const Rect& r = placed.rect;
    if (active && spec.widget != Widget::Lane) p.round_rect(Rect{r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 8, panel_hi);
    if (spec.widget == Widget::Lane) { draw_lane(p, spec, r, accent, relevant, active); return; }
    const uint32_t label = relevant ? (active ? text : soft) : faint;
    p.text_centered(r.x + r.w / 2, r.y + 28, host_.short_label(spec.param), label, r.w - 14, TextSize::Small);
    const double value = host_.value(spec.param);
    char formatted[96]{};
    host_.format(spec.param, value, formatted, sizeof(formatted));
    const ParamInfo info = host_.info(spec.param);
    switch (spec.widget) {
      case Widget::Knob: draw_knob(p, r, info, value, accent, relevant, active, formatted); break;
      case Widget::Toggle: {
        const bool on = value >= (info.min + info.max) * 0.5;
        const Rect pill{r.x + r.w / 2 - 32, r.y + 52, 64, 30};
        p.round_rect(pill, 15, on ? accent : border);
        p.circle(on ? pill.right() - 27 : pill.x + 3, pill.y + 3, 24, on ? bg : (relevant ? soft : faint));
        p.text_centered(r.x + r.w / 2, r.y + 116, formatted, relevant ? (on ? text : muted) : faint, r.w - 12, TextSize::Small);
        break;
      }
      case Widget::Segments: {
        const Rect row = segments_rect(r);
        const int count = option_count(spec.param);
        const int selected = static_cast<int>(std::lround(value - info.min));
        for (int i = 0; i < count; ++i) {
          const int x0 = row.x + i * row.w / count, x1 = row.x + (i + 1) * row.w / count;
          const bool on = i == selected;
          const bool hot = is_hovered(placed) && hover_.sub == i;
          p.round_rect(Rect{x0 + 2, row.y, x1 - x0 - 4, row.h}, 7, on ? accent : (hot ? raised : panel_hi));
          char option[64]{};
          host_.format(spec.param, info.min + i, option, sizeof(option));
          p.text_centered((x0 + x1) / 2, row.y + 27, option, on ? bg : (relevant ? soft : faint), x1 - x0 - 10, TextSize::Small);
        }
        break;
      }
      case Widget::Menu: {
        const Rect box = menu_box_rect(r);
        const bool open = menu_.param == spec.param;
        p.round_rect(box, 7, open || active ? raised : panel_hi);
        p.text(box.x + 14, box.y + 28, formatted, relevant ? text : faint, box.w - 50, TextSize::Small);
        draw_chevron(p, box.right() - 22, box.y + 19, open || active ? accent : muted);
        break;
      }
      default: break;
    }
  }

  void draw_knob(Painter& p, const Rect& r, const ParamInfo& info, double value, uint32_t accent,
                 bool relevant, bool active, const char* formatted) {
    using namespace theme;
    const int cx = r.x + r.w / 2, cy = r.y + 68, radius = 26;
    const double span = info.max - info.min;
    const auto norm_of = [&](double v) { return span > 0.0 ? std::clamp((v - info.min) / span, 0.0, 1.0) : 0.0; };
    const double start = 2.35619449, sweep = 4.71238898;  // 135 degrees, 270 degrees
    const auto arc = [&](double a0, double a1) {
      std::vector<Point> points;
      const int n = std::max(2, static_cast<int>(std::abs(a1 - a0) / 0.12) + 2);
      for (int i = 0; i < n; ++i) {
        const double a = a0 + (a1 - a0) * i / (n - 1);
        points.push_back({cx + static_cast<int>(std::lround(std::cos(a) * radius)),
                          cy + static_cast<int>(std::lround(std::sin(a) * radius))});
      }
      return points;
    };
    p.polyline(arc(start, start + sweep), border, 5);
    // Bipolar ranges fill from zero, so a centred transpose or detune reads as "nothing".
    const double origin = info.min < 0.0 && info.max > 0.0 ? norm_of(0.0) : 0.0;
    const double n = norm_of(value);
    if (std::abs(n - origin) > 0.004)
      p.polyline(arc(start + sweep * std::min(origin, n), start + sweep * std::max(origin, n)), accent, 5);
    p.circle(cx - radius + 8, cy - radius + 8, (radius - 8) * 2, active ? raised : panel_hi);
    const double a = start + sweep * n;
    p.line(cx + static_cast<int>(std::lround(std::cos(a) * 6)), cy + static_cast<int>(std::lround(std::sin(a) * 6)),
           cx + static_cast<int>(std::lround(std::cos(a) * (radius - 9))),
           cy + static_cast<int>(std::lround(std::sin(a) * (radius - 9))), relevant ? text : muted, 3);
    const double d = start + sweep * norm_of(info.def);
    p.line(cx + static_cast<int>(std::lround(std::cos(d) * (radius + 5))), cy + static_cast<int>(std::lround(std::sin(d) * (radius + 5))),
           cx + static_cast<int>(std::lround(std::cos(d) * (radius + 9))), cy + static_cast<int>(std::lround(std::sin(d) * (radius + 9))),
           muted, 2);
    p.text_centered(cx, r.y + 116, formatted, relevant ? (active ? amber : text) : faint, r.w - 12, TextSize::Small);
  }

  void draw_lane(Painter& p, const ControlSpec& spec, const Rect& r, uint32_t accent, bool relevant, bool active) {
    using namespace theme;
    const LaneGeometry g = lane_geometry(r);
    const ParamInfo info = host_.info(spec.param);
    const int length = spec.length >= 0 ? static_cast<int>(std::lround(host_.value(spec.length))) : spec.steps;
    const int step_w = g.step_width(spec.steps);
    const bool bipolar = info.min < 0.0;
    const bool levels = !bipolar && info.max - info.min <= 8.0;
    p.text(r.x + 4, r.y + 24, spec.caption ? spec.caption : host_.short_label(spec.param), relevant ? soft : faint, r.w / 2, TextSize::Small);
    p.text_right(r.right() - 8, r.y + 24, relevant ? "drag to paint  •  right-drag for a line" : "inactive", faint, r.w / 2 - 20, TextSize::Small);
    p.rect(g.bars, bg);
    // Axis: the discrete levels for a duty lane, or the range and zero line for pitch.
    if (levels) {
      const int count = static_cast<int>(std::lround(info.max - info.min)) + 1;
      for (int i = 0; i < count; ++i) {
        const int y0 = g.bars.bottom() - (i + 1) * g.bars.h / count;
        p.rect(g.bars.x, y0, g.bars.w, 1, border);
        char name[32]{};
        host_.format(spec.param, info.min + i, name, sizeof(name));
        p.text_right(g.bars.x - 8, y0 + g.bars.h / count / 2 + 7, name, muted, g.gutter - 10, TextSize::Small);
      }
    } else {
      char top[16]{}, bottom[16]{};
      std::snprintf(top, sizeof(top), "%+.0f", info.max);
      std::snprintf(bottom, sizeof(bottom), "%+.0f", info.min);
      p.text_right(g.bars.x - 8, g.bars.y + 16, top, muted, g.gutter - 10, TextSize::Small);
      p.text_right(g.bars.x - 8, g.bars.bottom() - 4, bottom, muted, g.gutter - 10, TextSize::Small);
    }
    const int zero_y = bipolar ? g.bars.y + static_cast<int>(std::lround(g.bars.h * info.max / (info.max - info.min))) : g.bars.bottom();
    if (bipolar) p.rect(g.bars.x, zero_y, g.bars.w, 1, muted);
    for (int i = 0; i < spec.steps; ++i) {
      const double value = host_.value(spec.param + i);
      const bool playing = i < length;
      const int x0 = g.bars.x + i * step_w + 3, w = step_w - 6;
      int top = zero_y, bottom = zero_y;
      if (levels) {
        const int count = static_cast<int>(std::lround(info.max - info.min)) + 1;
        top = g.bars.bottom() - static_cast<int>(std::lround((value - info.min + 1) * g.bars.h / count)) + 2;
      } else {
        const int y = g.bars.y + static_cast<int>(std::lround((info.max - value) / (info.max - info.min) * g.bars.h));
        top = std::min(y, zero_y); bottom = std::max(y, zero_y);
        if (bottom - top < 3) { top = zero_y - 1; bottom = zero_y + 2; }
      }
      const uint32_t bar = playing ? accent : mix(bg, accent, 0.28);
      p.rect(x0, top, w, bottom - top, bar);
      p.rect(x0, top, w, 3, playing ? text : mix(bg, text, 0.3));
      char label[32]{};
      if (levels) host_.format(spec.param + i, value, label, sizeof(label));
      else if (value == 0.0) std::snprintf(label, sizeof(label), "0");
      else std::snprintf(label, sizeof(label), "%+.0f", value);
      const int label_y = levels ? std::max(top + 22, g.bars.y + 20) : (value >= 0 ? top - 6 : bottom + 20);
      p.text_centered(x0 + w / 2, std::clamp(label_y, g.bars.y + 18, g.bars.bottom() - 4), label,
                      playing ? (levels ? bg : text) : muted, w - 4, TextSize::Small);
    }
    // Ruler: step numbers, the playing region, and a handle at the end of the loop.
    p.rect(g.ruler, panel_hi);
    p.rect(g.ruler.x, g.ruler.y, std::min(length, spec.steps) * step_w, g.ruler.h, mix(panel_hi, accent, 0.35));
    for (int i = 0; i < spec.steps; ++i) {
      char n[16]{};
      std::snprintf(n, sizeof(n), "%d", i + 1);
      p.text_centered(g.ruler.x + i * step_w + step_w / 2, g.ruler.y + 21, n, i < length ? text : muted, step_w, TextSize::Small);
    }
    const int handle = g.ruler.x + std::min(length, spec.steps) * step_w;
    p.rect(handle - 2, g.bars.y, 4, g.ruler.bottom() - g.bars.y, active ? amber : soft);
  }

  void draw_footer(Painter& p) {
    using namespace theme;
    p.round_rect(footer_rect, 8, panel);
    char line[640]{};
    int param = drag_.param >= 0 ? drag_.param : -1;
    if (param < 0 && menu_.param >= 0) param = menu_.param;
    if (param < 0 && (hover_.target == Target::Control || hover_.target == Target::Header)) param = hover_.param;
    if (param >= 0) {
      char value[96]{};
      host_.format(param, host_.value(param), value, sizeof(value));
      const char* how = how_to(param);
      std::snprintf(line, sizeof(line), "%s: %s  —  %s%s   %s", host_.label(param), value, host_.help(param),
                    host_.relevant(param) ? "" : " (inactive for this sound source)", how);
      p.text(footer_rect.x + 18, footer_rect.y + 27, line, text, footer_rect.w - 36, TextSize::Small);
      return;
    }
    const char* help = pages_[static_cast<size_t>(page_)].help;
    if (hover_.target == Target::Strip) if (const char* strip = host_.strip_help(page_)) help = strip;
    if (hover_.target == Target::Section) help = "Click a section title to collapse or expand it.";
    if (hover_.target == Target::Size)
      help = "Editor size  —  click steps smaller (wraps to largest)  •  wheel resizes  •  right-click resets to 100%  •  dragging the window edge works too";
    p.text(footer_rect.x + 18, footer_rect.y + 27, help, muted, footer_rect.w - 36, TextSize::Small);
  }

  // Usage hint for whichever control the footer is describing.
  const char* how_to(int param) const {
    if (menu_.param >= 0 || hover_.target == Target::Header) return "click to choose  •  wheel steps  •  right-click resets";
    Widget widget = Widget::Knob;
    if (drag_.param >= 0) widget = drag_.widget;
    else if (hover_.target == Target::Control) widget = control_spec(hover_.index, hover_.control).widget;
    switch (widget) {
      case Widget::Toggle: return "click toggles  •  right-click resets";
      case Widget::Segments: return "click to choose  •  wheel steps  •  right-click resets";
      case Widget::Menu: return "click for the list  •  wheel steps  •  right-click resets";
      case Widget::Lane:
        return hover_.ruler || (drag_.param >= 0 && drag_.param == drag_.lane.length)
            ? "click or drag the ruler to set how many steps play"
            : "drag paints steps  •  right-drag draws a line  •  right-click resets a step";
      default: (void)param; return "drag up or down  •  Shift for fine  •  double-click resets  •  wheel nudges";
    }
  }

  void draw_menu(Canvas& canvas) {
    using namespace theme;
    Painter p(canvas);
    p.fade = ease(menu_.fade);
    p.dy = static_cast<int>(std::lround((1.0 - p.fade) * -6.0));
    const Rect shadow{menu_.rect.x + 6, menu_.rect.y + 8, menu_.rect.w, menu_.rect.h};
    p.round_rect(shadow, 10, 0x05080c);
    p.round_rect(menu_.rect, 10, raised);
    const int count = option_count(menu_.param);
    const ParamInfo info = host_.info(menu_.param);
    const int selected = static_cast<int>(std::lround(host_.value(menu_.param) - info.min));
    for (int i = 0; i < count; ++i) {
      const Rect item = menu_item_rect(i);
      if (i == menu_.hover) p.round_rect(item, 6, 0x2c4260);
      if (i == selected) p.rect(item.x, item.y + 6, 4, item.h - 12, pages_[static_cast<size_t>(page_)].accent);
      char name[96]{};
      host_.format(menu_.param, info.min + i, name, sizeof(name));
      p.text(item.x + 14, item.y + 23, name, i == selected ? text : soft, item.w - 20, TextSize::Small);
    }
  }

  EditorHost& host_;
  std::vector<PageSpec> pages_;
  ControlSpec header_control_;
  int page_ = 0;
  double page_fade_ = 1.0;
  double tab_position_ = 0.0;
  std::vector<std::vector<double>> openness_;
  std::vector<std::vector<bool>> open_target_;
  double last_tick_ = -1.0;
  bool animating_ = false;
  Hit hover_{};
  Hit last_click_{};
  double last_click_time_ = -1.0e9;
  Drag drag_{};
  Menu menu_{};
};

}  // namespace yanes::ui
