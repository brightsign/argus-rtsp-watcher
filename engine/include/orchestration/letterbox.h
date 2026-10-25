#pragma once

#include <algorithm>
#include <cassert>

// Single source of truth for letterbox geometry: how a source frame is scaled
// and centered inside a destination of a different aspect ratio. The resize
// that feeds the NPU and every de-letterbox that maps detections back to
// camera pixels must agree to the pixel, so they all derive from compute().
// Header-only and dependency-free (no OpenCV/NPU) so it can be unit tested on
// the host, mirroring orchestration/zone.h.
namespace letterbox {

struct Geometry {
  float scale{1.0f};
  // Scaled source size inside the destination, truncated to whole pixels.
  int content_w{0};
  int content_h{0};
  // Top-left of the content inside the destination (the black bars).
  int pad_x{0};
  int pad_y{0};

  // Destination (letterboxed) pixel -> source pixel.
  float to_source_x(float dest_x) const noexcept { return (dest_x - pad_x) / scale; }
  float to_source_y(float dest_y) const noexcept { return (dest_y - pad_y) / scale; }

  // Source pixel -> destination (letterboxed) pixel.
  float to_dest_x(float source_x) const noexcept { return source_x * scale + pad_x; }
  float to_dest_y(float source_y) const noexcept { return source_y * scale + pad_y; }
};

// Aspect-preserving fit of a source_width x source_height frame into a
// dest_width x dest_height canvas, centered. Truncates (not rounds) the
// content size and the padding, matching the engine's historical behavior.
inline Geometry compute(int source_width, int source_height,
                        int dest_width, int dest_height) noexcept {
  assert(source_width > 0 && source_height > 0);
  assert(dest_width > 0 && dest_height > 0);

  Geometry geometry;
  geometry.scale = std::min(static_cast<float>(dest_width) / source_width,
                            static_cast<float>(dest_height) / source_height);
  geometry.content_w = static_cast<int>(source_width * geometry.scale);
  geometry.content_h = static_cast<int>(source_height * geometry.scale);
  geometry.pad_x = (dest_width - geometry.content_w) / 2;
  geometry.pad_y = (dest_height - geometry.content_h) / 2;
  return geometry;
}

}  // namespace letterbox
