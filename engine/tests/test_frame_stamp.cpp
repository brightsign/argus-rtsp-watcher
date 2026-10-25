/**
 * @file test_frame_stamp.cpp
 * @brief Unit tests for format_frame_stamp_text(), the pure formatter behind the
 * human-readable frame stamp (see orchestration/frame_stamp.h and
 * draw_frame_stamp() in orchestration/inference_worker.cpp, which draws it).
 */

#include <gtest/gtest.h>
#include "orchestration/frame_stamp.h"

using inference_worker::format_frame_stamp_text;

namespace {

// 2026-10-25T13:29:53.925Z, computed independently of the code under test.
constexpr int64_t kWallNs = 1792934993925000000LL;
constexpr int64_t kMillisecondNs = 1000000LL;

}  // namespace

TEST(FrameStampText, FormatsSequenceAndUtcWithMilliseconds) {
    EXPECT_EQ(format_frame_stamp_text(14984, kWallNs, 0), "#14984  2026-10-25T13:29:53.925Z");
}

TEST(FrameStampText, PadsMillisecondsAndFormatsTheEpoch) {
    EXPECT_EQ(format_frame_stamp_text(0, 5 * kMillisecondNs, 0), "#0  1970-01-01T00:00:00.005Z");
}

TEST(FrameStampText, ShowsLagWhenCameraTimeIsKnown) {
    // The camera captured the frame 55 ms before the engine received it.
    EXPECT_EQ(format_frame_stamp_text(14984, kWallNs, kWallNs - 55 * kMillisecondNs),
              "#14984  2026-10-25T13:29:53.925Z  lag 55ms");
}

TEST(FrameStampText, ShowsNegativeLagWhenTheCameraClockIsAhead) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs + 12 * kMillisecondNs),
              "#1  2026-10-25T13:29:53.925Z  lag -12ms");
}

TEST(FrameStampText, TruncatesSubMillisecondLag) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs - 55900000LL),
              "#1  2026-10-25T13:29:53.925Z  lag 55ms");
}

TEST(FrameStampText, ZeroLagIsStillShownWhenCameraTimeIsKnown) {
    // Lag 0 is a real measurement (it is also what a proxy that re-times the
    // stream would show), and must not be confused with "unavailable".
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs), "#1  2026-10-25T13:29:53.925Z  lag 0ms");
}

TEST(FrameStampText, NoLagWhenCameraTimeIsUnavailable) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, 0), "#1  2026-10-25T13:29:53.925Z");
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, -1), "#1  2026-10-25T13:29:53.925Z");
}

TEST(FrameStampText, LargeSequenceNumbersPrintInFull) {
    EXPECT_EQ(format_frame_stamp_text(UINT64_MAX, kWallNs, 0),
              "#18446744073709551615  2026-10-25T13:29:53.925Z");
}

TEST(FrameStampText, ShowsTheRunningAverageAfterTheLag) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs - 55 * kMillisecondNs, 61),
              "#1  2026-10-25T13:29:53.925Z  lag 55ms  avg15s 61ms");
}

TEST(FrameStampText, ShowsANegativeAverage) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs + 12 * kMillisecondNs, -12),
              "#1  2026-10-25T13:29:53.925Z  lag -12ms  avg15s -12ms");
}

TEST(FrameStampText, ZeroAverageIsStillShown) {
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, kWallNs, 0),
              "#1  2026-10-25T13:29:53.925Z  lag 0ms  avg15s 0ms");
}

TEST(FrameStampText, NoAverageWithoutTheCurrentLag) {
    // With no camera time for this frame there is no lag to put the average
    // beside, and a leftover average from earlier frames would read as current.
    EXPECT_EQ(format_frame_stamp_text(1, kWallNs, 0, 61), "#1  2026-10-25T13:29:53.925Z");
}

TEST(FrameStampText, AverageLabelNamesTheWindowItUses) {
    const std::string label = "avg" + std::to_string(metrics::kLagAverageWindowSeconds) + "s";
    EXPECT_NE(format_frame_stamp_text(1, kWallNs, kWallNs, 5).find(label), std::string::npos);
}
