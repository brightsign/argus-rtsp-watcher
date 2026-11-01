#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>

// Caps how often an inference worker may start a frame. The workers used to take
// every frame offered, so a stream configured for 10 fps ran at whatever the NPU
// and CPU allowed (11 to 15 fps measured), keeping the NPU busier than asked and
// every frame's inference slower for it.
//
// The caller asks how long to wait, sleeps that long, and only then takes the
// newest frame: a frame picked up at the start of its slot is as fresh as it
// can be, where one taken the moment the previous frame finished may have been
// waiting. Header-only and dependency-free so it is unit tested on the host.
namespace pipeline {

constexpr int64_t kNanosecondsPerSecond = 1000000000LL;

class FrameRateLimiter {
 public:
  // target_fps of 0 means no limit.
  explicit FrameRateLimiter(int target_fps)
      : interval_ns_(target_fps > 0 ? kNanosecondsPerSecond / target_fps : 0) {
    assert(target_fps >= 0);
  }

  // Nanoseconds still to wait before the next frame may start; 0 when it may
  // start now. Spacing is measured start to start, so a frame that overruns its
  // slot delays the next one only until it ends: there is no catch-up burst.
  int64_t wait_ns(int64_t now_ns) const noexcept {
    if (interval_ns_ == 0 || !started_) return 0;
    return std::max<int64_t>(0, last_start_ns_ + interval_ns_ - now_ns);
  }

  // A frame started at now_ns (its actual start, not the slot it was due in).
  void mark_started(int64_t now_ns) noexcept {
    last_start_ns_ = now_ns;
    started_ = true;
  }

  int64_t interval_ns() const noexcept { return interval_ns_; }

 private:
  int64_t interval_ns_;
  int64_t last_start_ns_{0};
  bool started_{false};
};

}  // namespace pipeline
