#pragma once

#include <algorithm>
#include <cmath>

namespace yanes::ui {

constexpr int width = 1600;
constexpr int height = 1050;
constexpr int pages = 5;
constexpr int rows_per_page = 16;
constexpr int tab_x=32,tab_y=72,tab_width=300,tab_height=54;
constexpr int visual_x=32,visual_y=142,visual_width=1536,visual_height=92;
constexpr int label_x=52,module_x=292,slider_x=470,slider_width=680,value_x=1180;
constexpr int rows_y=254,row_height=45,rail_y_offset=14,rail_height=16;
constexpr int sequence_x=470,sequence_cell=116,mixer_x=470,mixer_cell=60,bank_x=1000,bank_cell=35;
constexpr int tooltip_x=32,tooltip_y=990,tooltip_width=1536,tooltip_height=42;
constexpr int minimum_width=960,minimum_height=630;
constexpr int base_font_pixels=32;

inline int scale_x(int logical,int window_width){return static_cast<int>(std::lround(static_cast<double>(logical)*window_width/width));}
inline int scale_y(int logical,int window_height){return static_cast<int>(std::lround(static_cast<double>(logical)*window_height/height));}
inline int unscale_x(int physical,int window_width){return window_width>0?static_cast<int>(std::lround(static_cast<double>(physical)*width/window_width)):0;}
inline int unscale_y(int physical,int window_height){return window_height>0?static_cast<int>(std::lround(static_cast<double>(physical)*height/window_height)):0;}
inline double uniform_scale(int window_width,int window_height){return std::min(static_cast<double>(window_width)/width,static_cast<double>(window_height)/height);}
inline int font_pixels(int window_width,int window_height){return std::clamp(static_cast<int>(std::lround(base_font_pixels*uniform_scale(window_width,window_height))),22,72);}

constexpr bool contains(int x, int y, int left, int top, int w, int h) {
  return x >= left && x < left + w && y >= top && y < top + h;
}

constexpr int tab_at(int x, int y) {
  if (!contains(x, y, tab_x, tab_y, tab_width * pages, tab_height)) return -1;
  return std::clamp((x - tab_x) / tab_width, 0, pages - 1);
}

constexpr int row_at(int y) {
  if (y < rows_y || y >= rows_y + rows_per_page * row_height) return -1;
  return (y - rows_y) / row_height;
}

constexpr int param_at(int page, int x, int y, int param_count) {
  const int row = row_at(y);
  const int id = page * rows_per_page + row;
  return row >= 0 && contains(x, y, slider_x, rows_y, slider_width, rows_per_page * row_height) &&
                 id >= 0 && id < param_count ? id : -1;
}

constexpr int param_row_at(int page, int x, int y, int param_count) {
  const int row = row_at(y);
  const int id = page * rows_per_page + row;
  return row >= 0 && contains(x, y, label_x - 12, rows_y, value_x + 360 - label_x,
                              rows_per_page * row_height) && id >= 0 && id < param_count ? id : -1;
}

// The value field doubles as a large previous/next target for stepped choices.
// Return -1 outside it, otherwise 0 for the left half and 1 for the right half.
constexpr int value_step_direction_at(int x, int y) {
  if (!contains(x, y, value_x - 12, rows_y, 370, rows_per_page * row_height)) return -1;
  return x < value_x - 12 + 185 ? 0 : 1;
}

inline double value_from_x(int x, double minimum, double maximum, bool stepped) {
  const double norm = std::clamp(static_cast<double>(x - slider_x) / slider_width, 0.0, 1.0);
  const double value = minimum + (maximum - minimum) * norm;
  return stepped ? std::round(value) : value;
}

constexpr int sequence_step_at(int x, int y) {
  return contains(x,y,sequence_x,visual_y,sequence_cell*8,visual_height)?(x-sequence_x)/sequence_cell:-1;
}

inline double sequence_pitch_at(int y) {
  const double norm = 1.0 - 2.0 * std::clamp(static_cast<double>(y - visual_y) / visual_height, 0.0, 1.0);
  return std::round(norm * 24.0);
}

constexpr int mixer_channel_at(int x, int y) {
  return contains(x,y,mixer_x,visual_y,mixer_cell*16,visual_height)?(x-mixer_x)/mixer_cell:-1;
}

constexpr int bank_slot_at(int x, int y) {
  return contains(x,y,bank_x,visual_y,bank_cell*16,visual_height)?(x-bank_x)/bank_cell:-1;
}

}  // namespace yanes::ui
