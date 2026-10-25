/**
 * @file test_frame_rate_limiter.cpp
 * @brief Unit tests for pipeline::FrameRateLimiter, the cap that makes an
 * inference worker honour its stream's target_fps (see
 * pipeline/frame_rate_limiter.h).
 */

#include <gtest/gtest.h>
#include "pipeline/frame_rate_limiter.h"

using pipeline::FrameRateLimiter;

namespace {

constexpr int64_t kMillisecondNs = 1000000LL;
constexpr int64_t kSecondNs = 1000000000LL;

}  // namespace

TEST(FrameRateLimiter, ZeroMeansUnlimited) {
    FrameRateLimiter limiter(0);
    limiter.mark_started(0);
    EXPECT_EQ(limiter.wait_ns(0), 0);
    EXPECT_EQ(limiter.wait_ns(1), 0);
    EXPECT_EQ(limiter.interval_ns(), 0);
}

TEST(FrameRateLimiter, TheFirstFrameNeverWaits) {
    FrameRateLimiter limiter(10);
    EXPECT_EQ(limiter.wait_ns(0), 0);
    EXPECT_EQ(limiter.wait_ns(123456789), 0);
}

TEST(FrameRateLimiter, TenFpsMeansAHundredMillisecondInterval) {
    FrameRateLimiter limiter(10);
    EXPECT_EQ(limiter.interval_ns(), 100 * kMillisecondNs);
}

TEST(FrameRateLimiter, WaitsOutTheRestOfTheSlot) {
    FrameRateLimiter limiter(10);
    limiter.mark_started(5 * kSecondNs);

    EXPECT_EQ(limiter.wait_ns(5 * kSecondNs), 100 * kMillisecondNs);
    EXPECT_EQ(limiter.wait_ns(5 * kSecondNs + 30 * kMillisecondNs), 70 * kMillisecondNs);
    EXPECT_EQ(limiter.wait_ns(5 * kSecondNs + 99 * kMillisecondNs), 1 * kMillisecondNs);
}

TEST(FrameRateLimiter, DoesNotWaitOnceTheSlotHasPassed) {
    FrameRateLimiter limiter(10);
    limiter.mark_started(0);

    EXPECT_EQ(limiter.wait_ns(100 * kMillisecondNs), 0);
    EXPECT_EQ(limiter.wait_ns(250 * kMillisecondNs), 0);
}

TEST(FrameRateLimiter, AFrameThatOverrunsItsSlotCausesNoCatchUpBurst) {
    // The first frame starts at 0 and the worker is busy until 450 ms (a stall).
    // The next frame may start at once, but the one after is spaced from THAT
    // start, not from the missed slots: two frames never start back to back.
    FrameRateLimiter limiter(10);
    limiter.mark_started(0);
    ASSERT_EQ(limiter.wait_ns(450 * kMillisecondNs), 0);

    limiter.mark_started(450 * kMillisecondNs);

    EXPECT_EQ(limiter.wait_ns(450 * kMillisecondNs), 100 * kMillisecondNs);
}

TEST(FrameRateLimiter, SpacesAStreamOfFramesAtTheTargetRate) {
    FrameRateLimiter limiter(10);
    int64_t now = 0;
    int64_t previous_start = -1;
    for (int frame = 0; frame < 20; ++frame) {
        now += limiter.wait_ns(now);
        if (previous_start >= 0) {
            EXPECT_EQ(now - previous_start, 100 * kMillisecondNs) << "frame " << frame;
        }
        limiter.mark_started(now);
        previous_start = now;
        now += 30 * kMillisecondNs;  // the frame's own work, shorter than the slot
    }
    EXPECT_EQ(previous_start, 19 * 100 * kMillisecondNs);
}

TEST(FrameRateLimiter, FramesSlowerThanTheTargetAreNeverDelayed) {
    // Work takes 130 ms against a 100 ms slot: the worker is the limit, and the
    // limiter must add nothing on top.
    FrameRateLimiter limiter(10);
    int64_t now = 0;
    for (int frame = 0; frame < 10; ++frame) {
        EXPECT_EQ(limiter.wait_ns(now), 0) << "frame " << frame;
        limiter.mark_started(now);
        now += 130 * kMillisecondNs;
    }
}

TEST(FrameRateLimiter, ThirtyFpsIntervalTruncatesToWholeNanoseconds) {
    FrameRateLimiter limiter(30);
    EXPECT_EQ(limiter.interval_ns(), 33333333);
}

TEST(FrameRateLimiter, OneFpsIsASecond) {
    FrameRateLimiter limiter(1);
    EXPECT_EQ(limiter.interval_ns(), kSecondNs);
}
