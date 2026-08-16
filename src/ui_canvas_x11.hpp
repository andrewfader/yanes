// X11/Xft implementation of the editor canvas.
#pragma once

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include <algorithm>
#include <vector>

#include "ui_canvas.hpp"
#include "ui_layout.hpp"

namespace yanes::ui {

class X11Canvas final : public Canvas {
 public:
  X11Canvas(Display* display, Drawable drawable, GC gc, XftDraw* xft, XftFont* font,
            int window_width, int window_height)
      : display_(display), drawable_(drawable), gc_(gc), xft_(xft), font_(font),
        window_width_(window_width), window_height_(window_height) {}

  void clear(uint32_t rgb) override {
    XSetForeground(display_, gc_, rgb);
    XFillRectangle(display_, drawable_, gc_, 0, 0, static_cast<unsigned>(window_width_),
                   static_cast<unsigned>(window_height_));
  }

  void fill_rect(int x, int y, int width, int height, uint32_t rgb) override {
    XSetForeground(display_, gc_, rgb);
    XFillRectangle(display_, drawable_, gc_, sx(x), sy(y),
                   static_cast<unsigned>(std::max(1, sx(width))),
                   static_cast<unsigned>(std::max(1, sy(height))));
  }

  void draw_line(int x1, int y1, int x2, int y2, uint32_t rgb) override {
    XSetForeground(display_, gc_, rgb);
    XDrawLine(display_, drawable_, gc_, sx(x1), sy(y1), sx(x2), sy(y2));
  }

  void draw_polyline(const Point* points, int count, uint32_t rgb) override {
    if (count < 2) return;
    std::vector<XPoint> converted(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      converted[static_cast<size_t>(i)].x = static_cast<short>(sx(points[i].x));
      converted[static_cast<size_t>(i)].y = static_cast<short>(sy(points[i].y));
    }
    XSetForeground(display_, gc_, rgb);
    XDrawLines(display_, drawable_, gc_, converted.data(), count, CoordModeOrigin);
  }

  void fill_circle(int x, int y, int diameter, uint32_t rgb) override {
    XSetForeground(display_, gc_, rgb);
    XFillArc(display_, drawable_, gc_, sx(x), sy(y), static_cast<unsigned>(std::max(1, sx(diameter))),
             static_cast<unsigned>(std::max(1, sy(diameter))), 0, 360 * 64);
  }

 protected:
  void draw_glyphs(int x, int y, const std::string& text, uint32_t rgb) override {
    if (!xft_ || !font_) return;
    XRenderColor render{static_cast<unsigned short>(red(rgb) * 257),
                        static_cast<unsigned short>(green(rgb) * 257),
                        static_cast<unsigned short>(blue(rgb) * 257), 65535};
    XftColor color{};
    Visual* visual = DefaultVisual(display_, DefaultScreen(display_));
    Colormap colormap = DefaultColormap(display_, DefaultScreen(display_));
    if (!XftColorAllocValue(display_, visual, colormap, &render, &color)) return;
    XftDrawStringUtf8(xft_, &color, font_, sx(x), sy(y),
                      reinterpret_cast<const FcChar8*>(text.data()), static_cast<int>(text.size()));
    XftColorFree(display_, visual, colormap, &color);
  }

  int measure_text(const std::string& text) override {
    if (!font_) return 0;
    XGlyphInfo extents{};
    XftTextExtentsUtf8(display_, font_, reinterpret_cast<const FcChar8*>(text.data()),
                       static_cast<int>(text.size()), &extents);
    return unscale_x(extents.width, window_width_);
  }

 private:
  int sx(int value) const { return scale_x(value, window_width_); }
  int sy(int value) const { return scale_y(value, window_height_); }

  Display* display_;
  Drawable drawable_;
  GC gc_;
  XftDraw* xft_;
  XftFont* font_;
  int window_width_;
  int window_height_;
};

}  // namespace yanes::ui
