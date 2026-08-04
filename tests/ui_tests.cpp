#include "ui_layout.hpp"

#include <cassert>
#include <array>
#include <cmath>

int main() {
  using namespace yanes::ui;
  static_assert(width >= 1600 && height >= 1050);
  static_assert(rows_y + rows_per_page * row_height < height);
  static_assert(slider_x + slider_width < value_x);
  static_assert(base_font_pixels>=32);
  assert(font_pixels(width,height)==32);
  assert(font_pixels(minimum_width,minimum_height)>=22);
  assert(font_pixels(width*2,height*2)==64);

  for(const auto size:std::array<std::array<int,2>,5>{{{960,630},{1280,720},{1600,1050},{1920,1200},{2200,900}}}){
    for(const int x:{0,tab_x,slider_x,value_x,width})assert(std::abs(unscale_x(scale_x(x,size[0]),size[0])-x)<=1);
    for(const int y:{0,tab_y,rows_y,height})assert(std::abs(unscale_y(scale_y(y,size[1]),size[1])-y)<=1);
    assert(uniform_scale(size[0],size[1])>0.0);
    assert(font_pixels(size[0],size[1])>=22);
  }

  for (int page = 0; page < pages; ++page) {
    const int x = tab_x + page * tab_width + tab_width / 2;
    assert(tab_at(x, tab_y + tab_height / 2) == page);
  }
  assert(tab_at(tab_x - 1, tab_y + 1) == -1);
  assert(tab_at(tab_x + pages * tab_width, tab_y + 1) == -1);
  assert(tab_at(tab_x + 1, tab_y - 1) == -1);

  constexpr int param_count = 79;
  bool reached[param_count]{};
  for (int page = 0; page < pages; ++page) {
    for (int row = 0; row < rows_per_page; ++row) {
      const int expected = page * rows_per_page + row;
      const int y = rows_y + row * row_height + row_height / 2;
      const int actual = param_at(page, slider_x + slider_width / 2, y, param_count);
      if (expected < param_count) { assert(actual == expected); reached[expected] = true; }
      else assert(actual == -1);
    }
  }
  for (bool visible : reached) assert(visible);
  assert(param_at(0, slider_x - 1, rows_y, param_count) == -1);
  assert(param_at(0, slider_x + slider_width, rows_y, param_count) == -1);
  assert(param_at(0, slider_x, rows_y - 1, param_count) == -1);
  assert(param_at(4, slider_x, rows_y + 15 * row_height, param_count) == -1);

  assert(value_from_x(slider_x, -10.0, 10.0, false) == -10.0);
  assert(value_from_x(slider_x + slider_width, -10.0, 10.0, false) == 10.0);
  assert(value_from_x(slider_x - 100, -10.0, 10.0, false) == -10.0);
  assert(value_from_x(slider_x + slider_width + 100, -10.0, 10.0, false) == 10.0);
  assert(value_from_x(slider_x + slider_width / 2, 0.0, 3.0, true) == 2.0);

  for (int step = 0; step < 8; ++step)
    assert(sequence_step_at(sequence_x+step*sequence_cell+sequence_cell/2,visual_y+visual_height/2)==step);
  assert(sequence_step_at(sequence_x-1,visual_y)==-1);
  assert(sequence_step_at(sequence_x+8*sequence_cell,visual_y)==-1);
  assert(sequence_pitch_at(visual_y) == 24.0);
  assert(sequence_pitch_at(visual_y + visual_height) == -24.0);
  assert(sequence_pitch_at(visual_y + visual_height / 2) == 0.0);

  for (int channel = 0; channel < 16; ++channel)
    assert(mixer_channel_at(mixer_x+channel*mixer_cell+mixer_cell/2,visual_y+20)==channel);
  assert(mixer_channel_at(mixer_x-1,visual_y)==-1);
  assert(mixer_channel_at(mixer_x+16*mixer_cell,visual_y)==-1);

  for (int slot = 0; slot < 16; ++slot)
    assert(bank_slot_at(bank_x+slot*bank_cell+bank_cell/2,visual_y+20)==slot);
  assert(bank_slot_at(bank_x-1,visual_y)==-1);
  assert(bank_slot_at(bank_x+16*bank_cell,visual_y)==-1);
}
