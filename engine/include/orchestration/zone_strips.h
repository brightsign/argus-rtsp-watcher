#pragma once

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

// The regions of a frame that a zone outline can touch. The faint outline is a
// thin rectangle blended at low alpha, and blending it used to clone and blend
// the whole frame (two full-frame passes), so its cost grew with the picture:
// 8 ms at 640x480 and over 17 ms at 1280x720, for a line a few pixels wide. The
// pixels the outline never touches blend to themselves, so blending only the
// strips around its edges gives the same picture for a small fraction of the
// work. Header-only and dependency-free (no OpenCV) so the geometry, above all
// that no outline pixel is missed, is unit tested on the host.
namespace zone_strips {

// A half-open pixel rectangle: columns [x, x+width), rows [y, y+height).
struct Rect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};
};

// Anti-aliased lines bleed a pixel or so past their nominal width; this keeps
// the strips clear of that edge so none of the stroke is cut off.
constexpr int kAntialiasMarginPx = 2;

// The farthest a stroke of the given thickness reaches from a rectangle edge:
// half the width, plus a pixel of anti-aliasing.
inline int stroke_reach(int thickness) { return thickness / 2 + 1; }

// The strips an outline of the given thickness, drawn on the rectangle
// (x0,y0)-(x1,y1) (both corners inclusive, any order), can touch, clipped to a
// canvas_width x canvas_height canvas. They are the outer box (the rectangle
// grown by a margin) minus the inner box (shrunk by the same margin): at most
// four strips, none overlapping, so no pixel is blended twice. When the zone is
// too small for an inner box the outer box is returned whole.
inline std::vector<Rect> outline_strips(int x0, int y0, int x1, int y1, int thickness,
                                        int canvas_width, int canvas_height) {
  assert(thickness >= 1);
  assert(canvas_width >= 0 && canvas_height >= 0);
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);

  const int margin = thickness + kAntialiasMarginPx;  // strictly more than stroke_reach()

  // Exclusive right/bottom edges, so a corner pixel (x1, y1) is inside.
  const int outer_left = x0 - margin;
  const int outer_top = y0 - margin;
  const int outer_right = x1 + margin + 1;
  const int outer_bottom = y1 + margin + 1;

  const int inner_left = x0 + margin;
  const int inner_top = y0 + margin;
  const int inner_right = x1 - margin + 1;
  const int inner_bottom = y1 - margin + 1;

  std::vector<Rect> strips;
  auto add = [&](int left, int top, int right, int bottom) {
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, canvas_width);
    bottom = std::min(bottom, canvas_height);
    if (right > left && bottom > top) strips.push_back(Rect{left, top, right - left, bottom - top});
  };

  const bool inner_is_empty = inner_left >= inner_right || inner_top >= inner_bottom;
  if (inner_is_empty) {
    add(outer_left, outer_top, outer_right, outer_bottom);
    return strips;
  }
  add(outer_left, outer_top, outer_right, inner_top);        // top band, full width
  add(outer_left, inner_bottom, outer_right, outer_bottom);  // bottom band, full width
  add(outer_left, inner_top, inner_left, inner_bottom);      // left band, between them
  add(inner_right, inner_top, outer_right, inner_bottom);    // right band, between them
  return strips;
}

}  // namespace zone_strips
