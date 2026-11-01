/**
 * @file test_windowed_average.cpp
 * @brief Unit tests for metrics::WindowedAverage, the sliding-window mean behind
 * the on-frame lag average (see metrics/windowed_average.h).
 */

#include <gtest/gtest.h>
#include "metrics/windowed_average.h"

namespace {

constexpr int64_t kSecondNs = 1000000000LL;
constexpr int64_t kMillisecondNs = 1000000LL;

}  // namespace

TEST(WindowedAverage, HasNoMeanBeforeTheFirstSample) {
    metrics::WindowedAverage average(15 * kSecondNs);
    EXPECT_FALSE(average.mean().has_value());
    EXPECT_EQ(average.count(), 0u);
}

TEST(WindowedAverage, MeanOfASingleSampleIsThatSample) {
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(0, 55);
    ASSERT_TRUE(average.mean().has_value());
    EXPECT_DOUBLE_EQ(*average.mean(), 55.0);
}

TEST(WindowedAverage, AveragesSamplesInsideTheWindow) {
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(1 * kSecondNs, 40);
    average.add(2 * kSecondNs, 60);
    average.add(3 * kSecondNs, 80);
    EXPECT_DOUBLE_EQ(*average.mean(), 60.0);
    EXPECT_EQ(average.count(), 3u);
}

TEST(WindowedAverage, AcceptsNegativeValues) {
    // A camera clock ahead of ours gives negative lag; it must average, not clamp.
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(1 * kSecondNs, -20);
    average.add(2 * kSecondNs, 10);
    EXPECT_DOUBLE_EQ(*average.mean(), -5.0);
}

TEST(WindowedAverage, DropsSamplesOlderThanTheWindow) {
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(0, 100);
    average.add(10 * kSecondNs, 200);
    average.add(20 * kSecondNs, 300);
    // At t=20s the window is (5s, 20s]: the t=0 sample is gone, 10s and 20s remain.
    EXPECT_EQ(average.count(), 2u);
    EXPECT_DOUBLE_EQ(*average.mean(), 250.0);
}

TEST(WindowedAverage, ASampleExactlyAWindowOldIsDropped) {
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(0, 100);
    average.add(15 * kSecondNs, 300);
    EXPECT_EQ(average.count(), 1u);
    EXPECT_DOUBLE_EQ(*average.mean(), 300.0);
}

TEST(WindowedAverage, ASampleJustInsideTheWindowIsKept) {
    metrics::WindowedAverage average(15 * kSecondNs);
    average.add(0, 100);
    average.add(15 * kSecondNs - 1, 300);
    EXPECT_EQ(average.count(), 2u);
    EXPECT_DOUBLE_EQ(*average.mean(), 200.0);
}

TEST(WindowedAverage, AGapLongerThanTheWindowLeavesOnlyTheNewSample) {
    // After a stall (camera time unavailable, reconnect) the stale average must
    // not linger: the first new sample is the whole window.
    metrics::WindowedAverage average(15 * kSecondNs);
    for (int64_t second = 0; second < 5; ++second) average.add(second * kSecondNs, 100);
    average.add(60 * kSecondNs, 40);
    EXPECT_EQ(average.count(), 1u);
    EXPECT_DOUBLE_EQ(*average.mean(), 40.0);
}

TEST(WindowedAverage, MatchesABruteForceMeanOverALongRun) {
    // 30 fps for two minutes against a 15 s window: the running sum must equal a
    // fresh sum over the surviving samples, i.e. no drift from add/evict.
    constexpr int64_t kFrameNs = kSecondNs / 30;
    constexpr int64_t kFrames = 30 * 120;
    metrics::WindowedAverage average(15 * kSecondNs);

    for (int64_t frame = 0; frame < kFrames; ++frame) {
        average.add(frame * kFrameNs, (frame * 37) % 211 - 40);  // deterministic, signed
    }

    const int64_t last_time = (kFrames - 1) * kFrameNs;
    int64_t sum = 0;
    int64_t count = 0;
    for (int64_t frame = 0; frame < kFrames; ++frame) {
        if (last_time - frame * kFrameNs < 15 * kSecondNs) {
            sum += (frame * 37) % 211 - 40;
            ++count;
        }
    }
    EXPECT_EQ(static_cast<int64_t>(average.count()), count);
    EXPECT_DOUBLE_EQ(*average.mean(), static_cast<double>(sum) / static_cast<double>(count));
}

TEST(WindowedAverage, WindowConstantsAgree) {
    EXPECT_EQ(metrics::kLagAverageWindowNs,
              metrics::kLagAverageWindowSeconds * 1000 * kMillisecondNs);
}
