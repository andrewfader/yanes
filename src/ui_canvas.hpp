// The drawing surface the editor paints on. The editor itself is written once, in terms of the
// fixed design space declared in ui_layout.hpp; each platform supplies a Canvas that scales those
// coordinates to a real window and issues the native drawing calls.
#pragma once

#include <cstdint>
#include <string>

namespace yanes::ui {

struct Point { int x, y; };

class Canvas {
 public:
  virtual ~Canvas() = default;

  // Fills the whole surface, whatever its physical size, before the editor draws over it.
  virtual void clear(uint32_t rgb) = 0;
  virtual void fill_rect(int x, int y, int width, int height, uint32_t rgb) = 0;
  virtual void draw_line(int x1, int y1, int x2, int y2, uint32_t rgb) = 0;
  virtual void draw_polyline(const Point* points, int count, uint32_t rgb) = 0;
  virtual void fill_circle(int x, int y, int diameter, uint32_t rgb) = 0;

  // Draws text with its baseline at (x, y). A positive max_width shortens the string with an
  // ellipsis until it fits, so a long preset or parameter name never runs into the next column.
  void draw_text(int x, int y, const char* text, uint32_t rgb, int max_width = 0) {
    if (!text || !*text) return;
    std::string rendered = text;
    if (max_width > 0 && measure_text(rendered) > max_width) {
      while (rendered.size() > 4) {
        rendered.pop_back();
        // Never cut a UTF-8 sequence in half: the labels carry bullets and em dashes.
        while (!rendered.empty() && (static_cast<unsigned char>(rendered.back()) & 0xc0U) == 0x80U)
          rendered.pop_back();
        if (measure_text(rendered + "...") <= max_width) { rendered += "..."; break; }
      }
    }
    draw_glyphs(x, y, rendered, rgb);
  }

 protected:
  virtual void draw_glyphs(int x, int y, const std::string& text, uint32_t rgb) = 0;
  // Width of `text` in design-space units, so callers can compare it with a layout constant.
  virtual int measure_text(const std::string& text) = 0;

  static int red(uint32_t rgb) { return static_cast<int>((rgb >> 16U) & 255U); }
  static int green(uint32_t rgb) { return static_cast<int>((rgb >> 8U) & 255U); }
  static int blue(uint32_t rgb) { return static_cast<int>(rgb & 255U); }
};

}  // namespace yanes::ui
