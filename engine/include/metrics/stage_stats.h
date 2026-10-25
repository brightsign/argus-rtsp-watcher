#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>

// Per-stage timing for hot loops. A loop accumulates one StageStats per stage
// and logs a single summary line per reporting window, instead of one line per
// frame. Header-only and dependency-free so it can be unit tested on the host,
// mirroring orchestration/zone.h.
namespace metrics {

constexpr double kNanosecondsPerMillisecond = 1.0e6;

inline int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Accumulates the durations of one pipeline stage over a reporting window.
class StageStats {
 public:
  void add_ns(int64_t duration_ns) noexcept {
    assert(duration_ns >= 0);
    ++count_;
    total_ns_ += duration_ns;
    if (duration_ns > max_ns_) max_ns_ = duration_ns;
  }

  uint64_t count() const noexcept { return count_; }

  // Mean over the window; 0 when nothing was recorded.
  double average_ms() const noexcept {
    if (count_ == 0) return 0.0;
    return static_cast<double>(total_ns_) / static_cast<double>(count_) / kNanosecondsPerMillisecond;
  }

  double max_ms() const noexcept { return static_cast<double>(max_ns_) / kNanosecondsPerMillisecond; }

  void reset() noexcept {
    count_ = 0;
    total_ns_ = 0;
    max_ns_ = 0;
  }

 private:
  uint64_t count_{0};
  int64_t total_ns_{0};
  int64_t max_ns_{0};
};

}  // namespace metrics
