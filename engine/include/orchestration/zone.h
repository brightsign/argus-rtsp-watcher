#pragma once

// Pure feet-in-zone predicate behind the per-track in_zone MQTT field. Kept
// header-only and dependency-free (no OpenCV/NPU) so it can be unit tested on
// the host, mirroring orchestration/frame_stamp.h.
namespace zone {

// Shared configuration for a custom detection zone (normalized 0..1
// fractions of frame size), threaded from AppConfig through PipelineConfig
// and MqttPublisherCfg down to the per-track feet_in_zone check.
struct ZoneCfg { bool enabled{false}; float x0{0}, y0{0}, x1{0}, y1{0}; };

// Returns true iff the box's feet -- bottom-center ((bx0+bx1)/2, by1) -- fall
// inside the normalized zone rect [zx0,zy0,zx1,zy1] (fractions of frame size).
// Inclusive on all edges. Returns false for a non-positive frame size.
inline bool feet_in_zone(float bx0, float bx1, float by1,
                         int frame_w, int frame_h,
                         float zx0, float zy0, float zx1, float zy1) noexcept {
  if (frame_w <= 0 || frame_h <= 0) return false;
  const float fx = ((bx0 + bx1) * 0.5f) / static_cast<float>(frame_w);
  const float fy = by1 / static_cast<float>(frame_h);
  return fx >= zx0 && fx <= zx1 && fy >= zy0 && fy <= zy1;
}

} // namespace zone
