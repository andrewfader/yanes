// GDI implementation of the editor canvas. The device context is expected to be the back buffer
// the window paints into, so nothing here flushes or presents anything.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "ui_canvas.hpp"
#include "ui_layout.hpp"

namespace yanes::ui {

class Win32Canvas final : public Canvas {
 public:
  Win32Canvas(HDC dc, int window_width, int window_height)
      : dc_(dc), window_width_(window_width), window_height_(window_height) {
    for (int i = 0; i < 3; ++i)
      fonts_[static_cast<size_t>(i)] = CreateFontW(-font_pixels(static_cast<TextSize>(i), window_width, window_height), 0, 0, 0,
                                                   i == 2 ? FW_BOLD : FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    previous_font_ = static_cast<HFONT>(SelectObject(dc_, fonts_[1]));
    SetBkMode(dc_, TRANSPARENT);
    SetTextAlign(dc_, TA_LEFT | TA_BASELINE);
  }

  ~Win32Canvas() override {
    SelectObject(dc_, previous_font_);
    for (HFONT font : fonts_) DeleteObject(font);
  }

  Win32Canvas(const Win32Canvas&) = delete;
  Win32Canvas& operator=(const Win32Canvas&) = delete;

  void clear(uint32_t rgb) override {
    const RECT area{0, 0, window_width_, window_height_};
    SetDCBrushColor(dc_, colorref(rgb));
    FillRect(dc_, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  }

  void fill_rect(int x, int y, int width, int height, uint32_t rgb) override {
    const RECT area{sx(x), sy(y), sx(x) + std::max(1, sx(width)), sy(y) + std::max(1, sy(height))};
    SetDCBrushColor(dc_, colorref(rgb));
    FillRect(dc_, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  }

  void draw_polyline(const Point* points, int count, uint32_t rgb, int thickness) override {
    if (count < 2) return;
    std::vector<POINT> converted(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
      converted[static_cast<size_t>(i)] = POINT{sx(points[i].x), sy(points[i].y)};
    const int width = std::max(1, static_cast<int>(std::lround(thickness * uniform_scale(window_width_, window_height_))));
    HPEN pen = width > 1 ? ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                        static_cast<DWORD>(width), &brush_of(rgb), 0, nullptr)
                         : CreatePen(PS_SOLID, 1, colorref(rgb));
    HGDIOBJ previous = SelectObject(dc_, pen);
    Polyline(dc_, converted.data(), count);
    SelectObject(dc_, previous);
    DeleteObject(pen);
  }

  void fill_circle(int x, int y, int diameter, uint32_t rgb) override {
    HBRUSH brush = CreateSolidBrush(colorref(rgb));
    HGDIOBJ previous_brush = SelectObject(dc_, brush);
    HGDIOBJ previous_pen = SelectObject(dc_, GetStockObject(NULL_PEN));
    Ellipse(dc_, sx(x), sy(y), sx(x + diameter), sy(y + diameter));
    SelectObject(dc_, previous_pen);
    SelectObject(dc_, previous_brush);
    DeleteObject(brush);
  }

 protected:
  void draw_glyphs(int x, int y, const std::string& text, uint32_t rgb, TextSize size) override {
    const std::wstring wide = widen(text);
    if (wide.empty()) return;
    SelectObject(dc_, fonts_[static_cast<size_t>(size)]);
    SetTextColor(dc_, colorref(rgb));
    TextOutW(dc_, sx(x), sy(y), wide.c_str(), static_cast<int>(wide.size()));
  }

  int measure_text(const std::string& text, TextSize size) override {
    const std::wstring wide = widen(text);
    SelectObject(dc_, fonts_[static_cast<size_t>(size)]);
    SIZE extent{};
    if (wide.empty() || !GetTextExtentPoint32W(dc_, wide.c_str(), static_cast<int>(wide.size()), &extent))
      return 0;
    return unscale_x(extent.cx, window_width_);
  }

 private:
  static COLORREF colorref(uint32_t rgb) {
    return RGB(red(rgb), green(rgb), blue(rgb));
  }
  LOGBRUSH& brush_of(uint32_t rgb) {
    brush_ = LOGBRUSH{BS_SOLID, colorref(rgb), 0};
    return brush_;
  }

  static std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
  }

  int sx(int value) const { return scale_x(value, window_width_); }
  int sy(int value) const { return scale_y(value, window_height_); }

  HDC dc_;
  int window_width_;
  int window_height_;
  std::array<HFONT, 3> fonts_{};
  LOGBRUSH brush_{};
  HFONT previous_font_{};
};

}  // namespace yanes::ui
