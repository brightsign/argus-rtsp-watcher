#ifndef PIPELINE_LATENCY_H
#define PIPELINE_LATENCY_H

#include <cstdint>
#include <optional>

// Camera-clock-independent media-chain latency. The RTSP capture measures how
// long a frame has sat inside the player's GStreamer pipeline by comparing the
// buffer's running-time PTS to the pipeline clock's running-time "now" -- both
// taken from the player's own clock. An unsynchronized or wrong camera clock
// therefore cannot distort it, unlike the RTCP sender-report "camera time" in
// camera_time.h, whose lag is player_clock - camera_clock and so reports the
// camera's clock offset rather than real transport+decode latency. Header-only
// and dependency-free (no GStreamer) so the arithmetic is unit tested on the host.
namespace pipeline_latency {

constexpr int64_t kNanosecondsPerMillisecond = 1000000LL;

// A buffer cannot be pulled before it was timestamped, and the two clock reads
// are at most a frame interval plus the pipeline's buffering apart; a gap beyond
// this means the PTS and the running-time are not in the same base (the clock is
// not yet selected, a segment reset, or a different stream) and the result would
// mislead, so it is reported as unavailable instead.
constexpr int64_t kMaxPlausibleLatencyNs = 30 * 1000000000LL;  // 30 s

// Nanoseconds a buffer has spent in the pipeline: its running-time PTS subtracted
// from the pipeline's running-time "now". Returns nullopt when the inputs are
// unusable. A small negative result is clock-read jitter and is clamped to 0,
// because a frame cannot be pulled before it was captured; a result past the
// plausibility bound in either direction is dropped as not comparable.
inline std::optional<int64_t> age_ns(int64_t running_now_ns, int64_t buffer_pts_ns) noexcept {
  if (running_now_ns < 0 || buffer_pts_ns < 0) return std::nullopt;
  const int64_t age = running_now_ns - buffer_pts_ns;
  if (age < -kMaxPlausibleLatencyNs || age > kMaxPlausibleLatencyNs) return std::nullopt;
  return age < 0 ? 0 : age;
}

}  // namespace pipeline_latency

#endif  // PIPELINE_LATENCY_H
