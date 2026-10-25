#ifndef SHARED_FRAME_H
#define SHARED_FRAME_H

#include <cstdint>
#include <optional>
#include <vector>
#include <memory>

/**
 * SharedFrame: A frame captured once and fanned out to multiple model workers.
 * 
 * Allocated once in capture thread, read-only for workers.
 * Wrapped in shared_ptr for safe access across threads.
 */
struct SharedFrame {
    int64_t  pts_ns{0};      ///< Presentation timestamp (nanoseconds)
    uint64_t seq{0};         ///< Sequence number for ordering
    int64_t  capture_wall_ns{0}; ///< CLOCK_REALTIME ns minted alongside seq; joins recorded video to MQTT by frame_seq/frame_ts
    int64_t  source_wall_ns{0};  ///< Camera's own capture time (Unix epoch ns) from RTCP; 0 = unavailable
    /// Running mean of camera-to-engine lag (ms) over the last metrics::kLagAverageWindowSeconds,
    /// across every captured frame including this one; empty when this frame has no camera time.
    std::optional<int64_t> lag_average_ms;

    int width{0};            ///< Frame width in pixels (preprocessed/resized)
    int height{0};           ///< Frame height in pixels (preprocessed/resized)
    
    int orig_width{0};       ///< Original camera/stream width before preprocessing
    int orig_height{0};      ///< Original camera/stream height before preprocessing

    // BGR24 packed, CPU memory (plane0 only, single-planar)
    // Captured once by capture thread, read-only for model threads
    std::vector<uint8_t> bgr;
};

#endif // SHARED_FRAME_H
