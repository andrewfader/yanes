// The editor's fixed design space and the mapping between it and a real window. Everything is
// laid out at width x height and each backend scales to the actual window size.
#pragma once

#include <algorithm>
#include <cmath>

#include "ui_canvas.hpp"

namespace yanes::ui {

constexpr int width = 1600;
constexpr int height = 1050;
constexpr int minimum_width = 960, minimum_height = 630;

inline int scale_x(int logical,int window_width){return static_cast<int>(std::lround(static_cast<double>(logical)*window_width/width));}
inline int scale_y(int logical,int window_height){return static_cast<int>(std::lround(static_cast<double>(logical)*window_height/height));}
inline int unscale_x(int physical,int window_width){return window_width>0?static_cast<int>(std::lround(static_cast<double>(physical)*width/window_width)):0;}
inline int unscale_y(int physical,int window_height){return window_height>0?static_cast<int>(std::lround(static_cast<double>(physical)*height/window_height)):0;}
inline double uniform_scale(int window_width,int window_height){return std::min(static_cast<double>(window_width)/width,static_cast<double>(window_height)/height);}

// Design-space type sizes. The physical size follows the window but never drops below what
// stays legible at the minimum editor size.
constexpr int text_pixels(TextSize size) {
  return size == TextSize::Small ? 19 : (size == TextSize::Large ? 34 : 24);
}
inline int font_pixels(TextSize size, int window_width, int window_height) {
  const int minimum = size == TextSize::Small ? 11 : (size == TextSize::Large ? 20 : 14);
  return std::clamp(static_cast<int>(std::lround(text_pixels(size) * uniform_scale(window_width, window_height))),
                    minimum, 96);
}

struct Rect {
  int x{}, y{}, w{}, h{};
  constexpr bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  constexpr int right() const { return x + w; }
  constexpr int bottom() const { return y + h; }
  constexpr bool overlaps(const Rect& o) const {
    return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }
};

// Fixed chrome. Page content flows between content_top (or below the page's strip) and
// content_bottom.
constexpr int margin = 32;
constexpr Rect header_rect{0, 0, width, 64};
constexpr Rect preset_rect{480, 12, 540, 44};
constexpr Rect meter_rect{1110, 16, 458, 34};
constexpr int tab_y = 76, tab_height = 46;
constexpr int tab_count = 6;
constexpr int tab_width = (width - 2 * margin) / tab_count;
constexpr int strip_y = 134;
constexpr int content_bottom = 988;
constexpr Rect footer_rect{margin, 998, width - 2 * margin, 40};

// Cards: a header row, then a grid of equally sized cells. A step lane takes a full-width row.
constexpr int column_gap = 16;
constexpr int card_gap = 14;
constexpr int card_header = 42;
constexpr int card_padding = 12;
constexpr int cell_target_width = 150;
constexpr int cell_height = 124;
constexpr int lane_height = 206;

constexpr Rect tab_rect(int index) {
  return Rect{margin + index * tab_width, tab_y, tab_width - 8, tab_height};
}

constexpr int tab_at(int x, int y) {
  for (int i = 0; i < tab_count; ++i) if (tab_rect(i).contains(x, y)) return i;
  return -1;
}

}  // namespace yanes::ui
