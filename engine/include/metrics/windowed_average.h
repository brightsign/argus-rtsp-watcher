#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

// A running mean over a sliding time window, for values that jitter from frame
// to frame (such as camera-to-engine lag) where the recent average says more
// than any single sample. Header-only and dependency-free so it can be unit
// tested on the host, mirroring metrics/stage_stats.h.
namespace metrics {

// How far back the on-frame lag average looks. The stamp label ("avg15s") is
// built from the seconds value so the two can never disagree.
constexpr int kLagAverageWindowSeconds = 15;
constexpr int64_t kLagAverageWindowNs = kLagAverageWindowSeconds * 1000000000LL;

// Mean of the values added within the last window_ns. A sample is dropped once
// it is a full window old (age >= window_ns), so the window is half-open:
// (now - window_ns, now]. Not thread-safe; use from one thread.
class WindowedAverage {
 public:
  explicit WindowedAverage(int64_t window_ns) : window_ns_(window_ns) { assert(window_ns > 0); }

  // time_ns must not go backwards (use a monotonic clock). The new sample is
  // always kept, so a mean exists as soon as one value has been added.
  void add(int64_t time_ns, int64_t value) {
    assert(samples_.empty() || time_ns >= samples_.back().time_ns);
    samples_.push_back(Sample{time_ns, value});
    sum_ += value;
    while (time_ns - samples_.front().time_ns >= window_ns_) {
      sum_ -= samples_.front().value;
      samples_.pop_front();
    }
  }

  // The mean of the samples currently in the window; empty before the first add.
  std::optional<double> mean() const {
    if (samples_.empty()) return std::nullopt;
    return static_cast<double>(sum_) / static_cast<double>(samples_.size());
  }

  size_t count() const { return samples_.size(); }

 private:
  struct Sample {
    int64_t time_ns;
    int64_t value;
  };

  int64_t window_ns_;
  std::deque<Sample> samples_;
  int64_t sum_{0};
};

}  // namespace metrics
