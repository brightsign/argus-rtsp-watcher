#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

#include "metrics/windowed_average.h"

// Pure formatter for the human-readable frame stamp burned into annotated
// frames by draw_frame_stamp() (engine/src/orchestration/inference_worker.cpp).
// Kept header-only and dependency-free (no OpenCV) so it can be unit tested on
// the host without pulling in the vision/NPU stack.
namespace inference_worker {

constexpr int64_t kNanosecondsPerMillisecond = 1000000LL;
constexpr int64_t kNanosecondsPerSecond = 1000000000LL;

// The human-readable stamp line: "#<seq>  <ISO-8601 UTC, ms>Z", with
// "  lag <ms>ms" appended when the camera's capture time is known, and
// "  avg15s <ms>ms" after it when a running average of that lag is supplied.
// The lag is the engine's receive time minus the camera's capture time
// (negative means the camera clock runs ahead of ours); sub-millisecond
// remainders are truncated. wall_ns and source_wall_ns are nanoseconds since the
// Unix epoch; a source_wall_ns of 0 means the camera time is unavailable, and
// then neither suffix is shown, whatever lag_average_ms holds, because an
// average with no current lag beside it would be stale.
inline std::string format_frame_stamp_text(uint64_t seq, int64_t wall_ns,
                                           int64_t source_wall_ns,
                                           std::optional<int64_t> lag_average_ms = std::nullopt) {
  assert(wall_ns >= 0);
  const time_t wall_secs = static_cast<time_t>(wall_ns / kNanosecondsPerSecond);
  const int wall_millis =
      static_cast<int>((wall_ns / kNanosecondsPerMillisecond) % 1000LL);
  struct tm tm_utc{};
  gmtime_r(&wall_secs, &tm_utc);

  char text[96];
  std::snprintf(text, sizeof(text), "#%llu  %04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                static_cast<unsigned long long>(seq),
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, wall_millis);
  std::string line = text;

  if (source_wall_ns > 0) {
    const long long lag_ms =
        static_cast<long long>((wall_ns - source_wall_ns) / kNanosecondsPerMillisecond);
    char lag[32];
    std::snprintf(lag, sizeof(lag), "  lag %lldms", lag_ms);
    line += lag;

    if (lag_average_ms) {
      char average[40];
      std::snprintf(average, sizeof(average), "  avg%ds %lldms",
                    metrics::kLagAverageWindowSeconds,
                    static_cast<long long>(*lag_average_ms));
      line += average;
    }
  }
  return line;
}

} // namespace inference_worker
