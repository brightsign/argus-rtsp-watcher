/**
 * @file test_stream_ready_backoff.cpp
 * @brief Unit tests for the rules that let the engine wait for an RTSP stream
 * that is not published yet, instead of giving up on hardware decode (see
 * input/stream_ready_backoff.h).
 */

#include <gtest/gtest.h>
#include "input/stream_ready_backoff.h"

using rtsp_open::is_stream_not_ready;
using rtsp_open::not_ready_retry_delay_ms;

TEST(StreamReadyBackoff, DelayDoublesFromTheInitialValue) {
    EXPECT_EQ(not_ready_retry_delay_ms(0, 0), 500);
    EXPECT_EQ(not_ready_retry_delay_ms(1, 500), 1000);
    EXPECT_EQ(not_ready_retry_delay_ms(2, 1500), 2000);
}

TEST(StreamReadyBackoff, DelayStopsGrowingAtTheCap) {
    EXPECT_EQ(not_ready_retry_delay_ms(3, 3500), rtsp_open::kNotReadyMaxDelayMs);
    EXPECT_EQ(not_ready_retry_delay_ms(10, 20000), rtsp_open::kNotReadyMaxDelayMs);
    EXPECT_EQ(not_ready_retry_delay_ms(1000, 20000), rtsp_open::kNotReadyMaxDelayMs);
}

TEST(StreamReadyBackoff, StopsOnceTheBudgetWouldBeOverrun) {
    // 500 ms still fits in the last half second of the budget, but not past it.
    EXPECT_EQ(not_ready_retry_delay_ms(0, rtsp_open::kNotReadyBudgetMs - 500), 500);
    EXPECT_FALSE(not_ready_retry_delay_ms(0, rtsp_open::kNotReadyBudgetMs - 499).has_value());
    EXPECT_FALSE(not_ready_retry_delay_ms(5, rtsp_open::kNotReadyBudgetMs).has_value());
}

TEST(StreamReadyBackoff, ABudgetCoversTheObservedMediamtxStartup) {
    // MediaMTX was seen to need about 25 s to publish a camera; the budget must
    // outlast that with room to spare, or startup would still fall back to
    // software decode.
    EXPECT_GE(rtsp_open::kNotReadyBudgetMs, 2 * 25000);
}

TEST(StreamReadyBackoff, TheFullScheduleStaysWithinTheBudget) {
    int waited = 0;
    int retries = 0;
    while (const auto delay = not_ready_retry_delay_ms(retries, waited)) {
        waited += *delay;
        ++retries;
        ASSERT_LT(retries, 1000) << "the schedule never ends";
    }
    EXPECT_LE(waited, rtsp_open::kNotReadyBudgetMs);
    EXPECT_GT(waited, rtsp_open::kNotReadyBudgetMs - rtsp_open::kNotReadyMaxDelayMs);
}

TEST(IsStreamNotReady, TheResourceNotFoundCodeIsEnough) {
    EXPECT_TRUE(is_stream_not_ready(true, ""));
    EXPECT_TRUE(is_stream_not_ready(true, "something unrelated"));
}

TEST(IsStreamNotReady, TheMessageTextIsAcceptedWhateverItsCase) {
    EXPECT_TRUE(is_stream_not_ready(false, "Not found"));
    EXPECT_TRUE(is_stream_not_ready(false, "Not Found"));
    EXPECT_TRUE(is_stream_not_ready(false, "NOT FOUND"));
    EXPECT_TRUE(is_stream_not_ready(false, "RTSP/1.0 404 Not Found"));
}

TEST(IsStreamNotReady, RealFailuresAreNotMistakenForOne) {
    EXPECT_FALSE(is_stream_not_ready(false, "Internal data stream error."));
    EXPECT_FALSE(is_stream_not_ready(false, "Could not connect to server"));
    EXPECT_FALSE(is_stream_not_ready(false, "Unauthorized"));
    EXPECT_FALSE(is_stream_not_ready(false, "Could not open resource for reading"));
    EXPECT_FALSE(is_stream_not_ready(false, ""));
}
