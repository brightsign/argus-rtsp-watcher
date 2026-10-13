#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

// Pure zone-membership predicates behind the per-track in_zone MQTT field. Kept
// header-only and dependency-free (no OpenCV/NPU) so it can be unit tested on
// the host, mirroring orchestration/frame_stamp.h.
namespace zone {

// How a track's bounding box is tested against the zone for the in_zone field.
//   Base: the box's feet -- bottom-center -- must fall inside the zone.
//   Any:  any part of the box overlapping the zone counts.
// Any is the default (more permissive); operators pick per stream via config.
enum class Mode { Any, Base };

// Shared configuration for a custom detection zone (normalized 0..1
// fractions of frame size), threaded from AppConfig through PipelineConfig
// and MqttPublisherCfg down to the per-track in_zone check.
struct ZoneCfg {
  bool enabled{false};
  Mode mode{Mode::Any};
  float x0{0}, y0{0}, x1{0}, y1{0};
  // Outline color for the drawn zone, as 0..255 channels (default white).
  int r{255}, g{255}, b{255};
  // Outline width in pixels (default 3).
  int thickness{3};
  // Red alert-border width in pixels (default 20), independent of the outline.
  int border_thickness{20};
  // Rolling window (minutes) for the on-frame zone-detect counter (default 15).
  int window_minutes{15};
};

// Parse "#RRGGBB" (leading '#' optional) into r,g,b (0..255). Returns true and
// writes the channels on success; returns false and leaves them unchanged for a
// malformed string, so callers keep their defaults.
inline bool parse_hex_color(const std::string& s, int& r, int& g, int& b) noexcept {
  const std::string hex = (!s.empty() && s[0] == '#') ? s.substr(1) : s;
  if (hex.size() != 6) return false;
  int v[6];
  for (int i = 0; i < 6; ++i) {
    const char c = hex[i];
    if (c >= '0' && c <= '9') v[i] = c - '0';
    else if (c >= 'a' && c <= 'f') v[i] = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v[i] = c - 'A' + 10;
    else return false;
  }
  r = v[0] * 16 + v[1];
  g = v[2] * 16 + v[3];
  b = v[4] * 16 + v[5];
  return true;
}

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

// Returns true iff any part of the box [bx0,by0,bx1,by1] (pixels) overlaps the
// normalized zone rect [zx0,zy0,zx1,zy1] (fractions of frame size). This is the
// standard axis-aligned rectangle-intersection test, inclusive on all edges.
// Returns false for a non-positive frame size.
inline bool any_in_zone(float bx0, float by0, float bx1, float by1,
                        int frame_w, int frame_h,
                        float zx0, float zy0, float zx1, float zy1) noexcept {
  if (frame_w <= 0 || frame_h <= 0) return false;
  const float nx0 = bx0 / static_cast<float>(frame_w);
  const float ny0 = by0 / static_cast<float>(frame_h);
  const float nx1 = bx1 / static_cast<float>(frame_w);
  const float ny1 = by1 / static_cast<float>(frame_h);
  return nx0 <= zx1 && nx1 >= zx0 && ny0 <= zy1 && ny1 >= zy0;
}

// Returns true iff the box is inside the zone under the given mode. The box is
// given in pixels as [bx0,by0,bx1,by1]; the zone is normalized 0..1.
inline bool box_in_zone(Mode mode,
                        float bx0, float by0, float bx1, float by1,
                        int frame_w, int frame_h,
                        float zx0, float zy0, float zx1, float zy1) noexcept {
  if (mode == Mode::Base) {
    return feet_in_zone(bx0, bx1, by1, frame_w, frame_h, zx0, zy0, zx1, zy1);
  }
  return any_in_zone(bx0, by0, bx1, by1, frame_w, frame_h, zx0, zy0, zx1, zy1);
}

// EventWindow is the rolling-window confirmation behind the low-latency in-zone
// flash event. It fires once at least min_hits frames carrying an in-zone
// detection fall within the last window_ms, then rate-limits further fires to
// one per repeat_ms while activity continues. Pure and stateful (no clock of its
// own; the caller passes a monotonic now_ms), so it is host unit-testable.
struct EventWindow {
  int64_t window_ms{1000};  // lookback window
  std::size_t min_hits{2};  // in-window frames needed to fire
  int64_t repeat_ms{500};   // minimum gap between fires

  std::deque<int64_t> hits;
  int64_t last_fire_ms{-1};  // -1 => never fired

  // record folds one frame in: in_zone marks whether any box was in the zone this
  // frame, now_ms is the current monotonic time. Returns true when an event
  // should be emitted for this frame.
  bool record(bool in_zone, int64_t now_ms) {
    if (in_zone) hits.push_back(now_ms);
    while (!hits.empty() && now_ms - hits.front() > window_ms) hits.pop_front();
    if (hits.size() < min_hits) return false;
    if (last_fire_ms >= 0 && now_ms - last_fire_ms < repeat_ms) return false;
    last_fire_ms = now_ms;
    return true;
  }

  // reset clears the window (e.g. when the zone is disabled).
  void reset() { hits.clear(); }
};

// DetectCounter tallies discrete zone-detect episodes over a rolling window. A
// detect is counted once when presence is first confirmed (EventWindow: two
// in-zone frames within 1s); continuous presence stays a single detect until the
// zone goes quiet for episode_gap_ms, after which a fresh entry counts again.
// count() returns how many detects fall within the last window_ms. Pure/stateful
// (caller supplies a monotonic now_ms), so it is host unit-testable.
struct DetectCounter {
  EventWindow confirm;               // presence confirmation (2 hits within 1s)
  int64_t episode_gap_ms{2000};      // quiet gap that ends an episode
  int64_t window_ms{15 * 60 * 1000}; // rolling count window (default 15 min)
  int64_t trigger_hold_ms{2000};     // alert-border hold: keep it solid across dropped detection frames

  bool active{false};
  int64_t last_in_zone_ms{-1};
  std::deque<int64_t> detects;       // timestamps of counted detects

  // record folds one frame in and returns the current rolling count. in_zone
  // marks whether any box was in the zone this frame.
  int record(bool in_zone, int64_t now_ms) {
    if (in_zone) last_in_zone_ms = now_ms;
    if (active && last_in_zone_ms >= 0 && now_ms - last_in_zone_ms > episode_gap_ms) {
      active = false;  // episode ended after a quiet gap
    }
    if (confirm.record(in_zone, now_ms) && !active) {
      active = true;
      detects.push_back(now_ms);
    }
    prune(now_ms);
    return static_cast<int>(detects.size());
  }

  // count prunes expired detects and returns the rolling total without recording.
  int count(int64_t now_ms) {
    prune(now_ms);
    return static_cast<int>(detects.size());
  }

  // triggered reports whether the zone is currently firing: true from the first
  // in-zone frame, held trigger_hold_ms past the last in-zone frame to bridge
  // dropped detections, and clears once the zone has been empty that long. Drives
  // the red alert border. Call after record() so last_in_zone_ms is current.
  bool triggered(int64_t now_ms) const {
    return last_in_zone_ms >= 0 && now_ms - last_in_zone_ms <= trigger_hold_ms;
  }

private:
  void prune(int64_t now_ms) {
    while (!detects.empty() && now_ms - detects.front() > window_ms) detects.pop_front();
  }
};

} // namespace zone
