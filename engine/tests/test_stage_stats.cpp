/**
 * @file test_stage_stats.cpp
 * @brief Unit tests for metrics::StageStats, the per-stage timing accumulator
 * the capture and inference loops use to log one summary line per window
 * (see metrics/stage_stats.h).
 */

#include <gtest/gtest.h>
#include "metrics/stage_stats.h"

namespace {

constexpr int64_t kOneMillisecondNs = 1000000;

}  // namespace

TEST(StageStats, EmptyWindowReportsZero) {
    metrics::StageStats stats;
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_DOUBLE_EQ(stats.average_ms(), 0.0);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 0.0);
}

TEST(StageStats, AverageAndMaxOverSamples) {
    metrics::StageStats stats;
    stats.add_ns(1 * kOneMillisecondNs);
    stats.add_ns(3 * kOneMillisecondNs);
    EXPECT_EQ(stats.count(), 2u);
    EXPECT_DOUBLE_EQ(stats.average_ms(), 2.0);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 3.0);
}

TEST(StageStats, MaxIsIndependentOfSampleOrder) {
    metrics::StageStats stats;
    stats.add_ns(5 * kOneMillisecondNs);
    stats.add_ns(1 * kOneMillisecondNs);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 5.0);
}

TEST(StageStats, ZeroDurationSampleStillCounts) {
    // A skipped frame (the writer drops two of every three) is a real sample
    // of zero cost and must pull the average down, not be ignored.
    metrics::StageStats stats;
    stats.add_ns(0);
    stats.add_ns(0);
    stats.add_ns(6 * kOneMillisecondNs);
    EXPECT_EQ(stats.count(), 3u);
    EXPECT_DOUBLE_EQ(stats.average_ms(), 2.0);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 6.0);
}

TEST(StageStats, ResetClearsTheWindow) {
    metrics::StageStats stats;
    stats.add_ns(9 * kOneMillisecondNs);
    stats.reset();
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_DOUBLE_EQ(stats.average_ms(), 0.0);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 0.0);
    // A reused window must not remember the previous maximum.
    stats.add_ns(1 * kOneMillisecondNs);
    EXPECT_DOUBLE_EQ(stats.max_ms(), 1.0);
}

TEST(StageStats, SteadyClockNeverGoesBackwards) {
    const int64_t first = metrics::steady_now_ns();
    const int64_t second = metrics::steady_now_ns();
    EXPECT_GE(second, first);
}
