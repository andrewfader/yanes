// The drawing surface the editor paints on. The editor itself is written once, in terms of the
// fixed design space declared in ui_layout.hpp; each platform supplies a Canvas that scales those
// coordinates to a real window and issues the native drawing calls.
#pragma once

#include <cstdint>
#include <string>

namespace yanes::ui {

struct Point { int x, y; };

// Three type sizes cover the editor: small for control labels and readouts, normal for values
// and section titles, large for the logo. Each backend keeps one font per size.
enum class TextSize : uint8_t { Small, Normal, Large };

class Canvas {
 public:
  virtual ~Canvas() = default;

  // Fills the whole surface, whatever its physical size, before the editor draws over it.
  virtual void clear(uint32_t rgb) = 0;
  virtual void fill_rect(int x, int y, int width, int height, uint32_t rgb) = 0;
  // Thickness is in design-space units and scales with the window like everything else.
  virtual void draw_polyline(const Point* points, int count, uint32_t rgb, int thickness = 1) = 0;
  virtual void fill_circle(int x, int y, int diameter, uint32_t rgb) = 0;

  void draw_line(int x1, int y1, int x2, int y2, uint32_t rgb, int thickness = 1) {
    const Point points[]{{x1, y1}, {x2, y2}};
    draw_polyline(points, 2, rgb, thickness);
  }

  // Draws text with its baseline at (x, y). A positive max_width shortens the string with an
  // ellipsis until it fits, so a long preset or parameter name never runs into the next column.
  void draw_text(int x, int y, const char* text, uint32_t rgb, int max_width = 0,
                 TextSize size = TextSize::Normal) {
    if (!text || !*text) return;
    draw_glyphs(x, y, fit(text, max_width, size), rgb, size);
  }

  // Width of `text` in design-space units, so callers can centre or right-align it.
  int text_width(const char* text, TextSize size = TextSize::Normal) {
    return text && *text ? measure_text(text, size) : 0;
  }

  // The string draw_text would actually render for this width.
  std::string fit(const char* text, int max_width, TextSize size) {
    std::string rendered = text ? text : "";
    if (max_width > 0 && measure_text(rendered, size) > max_width) {
      if (measure_text("...", size) > max_width) return {};
      while (!rendered.empty()) {
        // Walk backward over continuation bytes, then remove the leading byte too.
        size_t last = rendered.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(rendered[last]) & 0xc0U) == 0x80U) --last;
        rendered.resize(last);
        if (measure_text(rendered + "...", size) <= max_width) return rendered + "...";
      }
    }
    return rendered;
  }

 protected:
  virtual void draw_glyphs(int x, int y, const std::string& text, uint32_t rgb, TextSize size) = 0;
  virtual int measure_text(const std::string& text, TextSize size) = 0;

  static int red(uint32_t rgb) { return static_cast<int>((rgb >> 16U) & 255U); }
  static int green(uint32_t rgb) { return static_cast<int>((rgb >> 8U) & 255U); }
  static int blue(uint32_t rgb) { return static_cast<int>(rgb & 255U); }
};

}  // namespace yanes::ui
