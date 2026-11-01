#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

// The pure parts of recovering a frame's camera capture time from the RTCP
// sender reports that ride alongside an RTSP stream. GStreamer attaches a
// reference timestamp (NTP time, taken from those reports) to the RTP buffers
// inside rtspsrc; input_rtsp.cpp reads it there and looks it up again at the
// sink by buffer PTS, because the decoder in between need not copy the meta.
// Header-only and dependency-free (no GStreamer) so it can be unit tested on
// the host, mirroring orchestration/zone.h.
namespace camera_time {

constexpr int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr int64_t kNanosecondsPerMillisecond = 1000000LL;

// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr int64_t kNtpToUnixEpochSeconds = 2208988800LL;

// A camera clock further than this from ours is unsynced (or its time zone is
// wrong); publishing that as a capture time would mislead, so it is dropped.
constexpr int64_t kMaxPlausibleSkewNs = 60 * kNanosecondsPerSecond;

// Number of distinct recent frame PTS values remembered. The sink lags the
// network by a few frames at most, so a short ring is enough.
constexpr size_t kReferenceRingCapacity = 64;

// GStreamer's timestamp/x-ntp reference timestamps count nanoseconds since the
// NTP epoch. Returns the same instant as nanoseconds since the Unix epoch.
inline int64_t ntp_ns_to_unix_ns(uint64_t ntp_ns) noexcept {
  assert(ntp_ns <= static_cast<uint64_t>(INT64_MAX));
  return static_cast<int64_t>(ntp_ns) - kNtpToUnixEpochSeconds * kNanosecondsPerSecond;
}

// True when source_unix_ns is within kMaxPlausibleSkewNs of the engine's own
// wall clock, in either direction (a camera clock ahead of ours is as wrong as
// one behind). A zero source time is "unavailable" and is never plausible.
inline bool is_plausible_source_time(int64_t source_unix_ns, int64_t engine_wall_ns) noexcept {
  if (source_unix_ns <= 0) return false;
  const int64_t skew = engine_wall_ns - source_unix_ns;
  return skew <= kMaxPlausibleSkewNs && skew >= -kMaxPlausibleSkewNs;
}

// Remembers the camera time of recent frames by buffer PTS. The RTP packets of
// one frame share a PTS, so a repeat of the most recent PTS is ignored rather
// than consuming a slot. Written from GStreamer's streaming thread and read
// from the capture thread, hence the mutex.
class ReferenceTimeRing {
 public:
  void record(int64_t pts_ns, int64_t unix_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0 && entries_[last_index()].pts_ns == pts_ns) return;
    entries_[next_] = Entry{pts_ns, unix_ns};
    next_ = (next_ + 1) % kReferenceRingCapacity;
    if (count_ < kReferenceRingCapacity) ++count_;
  }

  // The camera time recorded for exactly pts_ns, if it is still remembered.
  std::optional<int64_t> lookup(int64_t pts_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].pts_ns == pts_ns) return entries_[i].unix_ns;
    }
    return std::nullopt;
  }

  // Forget everything: a new pipeline restarts the PTS clock.
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    count_ = 0;
    next_ = 0;
  }

 private:
  struct Entry {
    int64_t pts_ns{0};
    int64_t unix_ns{0};
  };

  size_t last_index() const {
    return (next_ + kReferenceRingCapacity - 1) % kReferenceRingCapacity;
  }

  mutable std::mutex mutex_;
  std::array<Entry, kReferenceRingCapacity> entries_{};
  size_t next_{0};
  size_t count_{0};
};

}  // namespace camera_time
