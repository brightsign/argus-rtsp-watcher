/**
 * @file test_pipeline_latency.cpp
 * @brief Unit tests for pipeline_latency::age_ns(), the pure arithmetic behind
 * the camera-clock-independent media-chain latency (see input/pipeline_latency.h;
 * input_rtsp.cpp computes running_now from the pipeline clock and the buffer PTS).
 */

#include <gtest/gtest.h>
#include "input/pipeline_latency.h"

using pipeline_latency::age_ns;
using pipeline_latency::kMaxPlausibleLatencyNs;

namespace {
constexpr int64_t kMs = pipeline_latency::kNanosecondsPerMillisecond;
}  // namespace

TEST(PipelineLatencyAge, ReturnsElapsedRunningTime) {
    // The frame was timestamped 142 ms (in running-time) before it was pulled.
    const int64_t pts = 1000 * kMs;
    const int64_t now = 1142 * kMs;
    ASSERT_TRUE(age_ns(now, pts).has_value());
    EXPECT_EQ(*age_ns(now, pts), 142 * kMs);
}

TEST(PipelineLatencyAge, ZeroWhenPulledAtItsTimestamp) {
    EXPECT_EQ(age_ns(5000 * kMs, 5000 * kMs), 0);
}

TEST(PipelineLatencyAge, ClampsSmallNegativeJitterToZero) {
    // The pipeline clock read can land a hair before the buffer's PTS; a true
    // negative age is impossible, so it reads as zero, not as a negative latency.
    EXPECT_EQ(age_ns(1000 * kMs - 3 * kMs, 1000 * kMs), 0);
}

TEST(PipelineLatencyAge, DropsImplausiblyLargeAge) {
    // A gap past the bound means PTS and running-time are not in the same base.
    EXPECT_FALSE(age_ns(kMaxPlausibleLatencyNs + 1, 0).has_value());
}

TEST(PipelineLatencyAge, DropsImplausiblyLargeNegativeGap) {
    EXPECT_FALSE(age_ns(0, kMaxPlausibleLatencyNs + 1).has_value());
}

TEST(PipelineLatencyAge, KeepsAgeExactlyAtTheBound) {
    ASSERT_TRUE(age_ns(kMaxPlausibleLatencyNs, 0).has_value());
    EXPECT_EQ(*age_ns(kMaxPlausibleLatencyNs, 0), kMaxPlausibleLatencyNs);
}

TEST(PipelineLatencyAge, RejectsNegativeInputs) {
    EXPECT_FALSE(age_ns(-1, 0).has_value());
    EXPECT_FALSE(age_ns(0, -1).has_value());
}
