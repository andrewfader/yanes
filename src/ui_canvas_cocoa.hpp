// AppKit implementation of the editor canvas. This header contains Objective-C++ and may only be
// included from a translation unit compiled as Objective-C++ (see the APPLE branch in
// CMakeLists.txt). It expects to run inside -drawRect: of a flipped view, so the coordinate system
// already matches the top-left origin the editor draws in.
#pragma once

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <array>

#include "ui_canvas.hpp"
#include "ui_layout.hpp"

namespace yanes::ui {

class CocoaCanvas final : public Canvas {
 public:
  CocoaCanvas(int window_width, int window_height)
      : window_width_(window_width), window_height_(window_height) {
    for (int i = 0; i < 3; ++i)
      fonts_[static_cast<size_t>(i)] =
          [NSFont systemFontOfSize:static_cast<CGFloat>(font_pixels(static_cast<TextSize>(i), window_width, window_height))
                            weight:i == 2 ? NSFontWeightBold : NSFontWeightMedium];
  }

  void clear(uint32_t rgb) override {
    [color(rgb) set];
    NSRectFill(NSMakeRect(0, 0, window_width_, window_height_));
  }

  void fill_rect(int x, int y, int width, int height, uint32_t rgb) override {
    [color(rgb) set];
    NSRectFill(NSMakeRect(sx(x), sy(y), std::max<CGFloat>(1.0, sx(width)),
                              std::max<CGFloat>(1.0, sy(height))));
  }

  void draw_polyline(const Point* points, int count, uint32_t rgb, int thickness) override {
    if (count < 2) return;
    [color(rgb) set];
    NSBezierPath* path = [NSBezierPath bezierPath];
    [path setLineWidth:std::max<CGFloat>(1.0, thickness * uniform_scale(window_width_, window_height_))];
    [path setLineCapStyle:NSLineCapStyleRound];
    [path setLineJoinStyle:NSLineJoinStyleRound];
    [path moveToPoint:NSMakePoint(sx(points[0].x), sy(points[0].y))];
    for (int i = 1; i < count; ++i)
      [path lineToPoint:NSMakePoint(sx(points[i].x), sy(points[i].y))];
    [path stroke];
  }

  void fill_circle(int x, int y, int diameter, uint32_t rgb) override {
    [color(rgb) set];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(
        sx(x), sy(y), std::max<CGFloat>(1.0, sx(diameter)),
        std::max<CGFloat>(1.0, sy(diameter)))] fill];
  }

 protected:
  void draw_glyphs(int x, int y, const std::string& text, uint32_t rgb, TextSize size) override {
    NSFont* font_ = fonts_[static_cast<size_t>(size)];
    NSString* string = to_string(text);
    if (!string) return;
    // The editor positions text by its baseline; AppKit draws from the top-left in a flipped view.
    [string drawAtPoint:NSMakePoint(sx(x), sy(y) - [font_ ascender])
         withAttributes:@{NSFontAttributeName : font_, NSForegroundColorAttributeName : color(rgb)}];
  }

  int measure_text(const std::string& text, TextSize size) override {
    NSFont* font_ = fonts_[static_cast<size_t>(size)];
    NSString* string = to_string(text);
    if (!string) return 0;
    const NSSize size = [string sizeWithAttributes:@{NSFontAttributeName : font_}];
    return unscale_x(static_cast<int>(size.width), window_width_);
  }

 private:
  static NSString* to_string(const std::string& text) {
    return [[[NSString alloc] initWithBytes:text.data()
                                     length:text.size()
                                   encoding:NSUTF8StringEncoding] autorelease];
  }

  static NSColor* color(uint32_t rgb) {
    return [NSColor colorWithSRGBRed:red(rgb) / 255.0 green:green(rgb) / 255.0
                                blue:blue(rgb) / 255.0 alpha:1.0];
  }

  CGFloat sx(int value) const { return scale_x(value, window_width_); }
  CGFloat sy(int value) const { return scale_y(value, window_height_); }

  int window_width_;
  int window_height_;
  std::array<NSFont*, 3> fonts_{};
};

}  // namespace yanes::ui
