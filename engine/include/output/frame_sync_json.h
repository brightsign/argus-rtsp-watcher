#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// The frame-identification fields of an analytics MQTT message, built as a
// pure function so the JSON shape is unit tested on the host (header-only, no
// MQTT or OpenCV dependency), mirroring orchestration/zone.h.
//
// Two different frames are named, and a consumer relating boxes to video must
// use the second:
//   frame_seq / frame_ts : the newest frame CAPTURED when the message was built.
//   det_seq / det_ts     : the frame the person boxes were detected on, which
//                          lags the newest capture by the inference delay.
//   source_ts            : the camera's own capture time of that detection
//                          frame, from RTCP sender reports; null when the camera
//                          sent none or its clock is implausible.
// All *_ts values are epoch milliseconds (CLOCK_REALTIME).
namespace output {

constexpr int64_t kNanosecondsPerMillisecond = 1000000LL;

// Returns the fields as `"frame_seq":..,"frame_ts":..,"det_seq":..,"det_ts":..,"source_ts":..`
// with no surrounding braces or trailing comma. det_seq 0 means no detection
// frame yet. A det_source_wall_ns of 0 means the camera time is unavailable.
inline std::string frame_sync_json(uint64_t frame_seq, int64_t frame_capture_wall_ns,
                                   uint64_t det_seq, int64_t det_capture_wall_ns,
                                   int64_t det_source_wall_ns) {
  char source_ts[32];
  if (det_source_wall_ns > 0) {
    std::snprintf(source_ts, sizeof(source_ts), "%lld",
                  static_cast<long long>(det_source_wall_ns / kNanosecondsPerMillisecond));
  } else {
    std::snprintf(source_ts, sizeof(source_ts), "null");
  }

  char fields[256];
  std::snprintf(fields, sizeof(fields),
                "\"frame_seq\":%llu,\"frame_ts\":%lld,"
                "\"det_seq\":%llu,\"det_ts\":%lld,\"source_ts\":%s",
                static_cast<unsigned long long>(frame_seq),
                static_cast<long long>(frame_capture_wall_ns / kNanosecondsPerMillisecond),
                static_cast<unsigned long long>(det_seq),
                static_cast<long long>(det_capture_wall_ns / kNanosecondsPerMillisecond),
                source_ts);
  return fields;
}

}  // namespace output
