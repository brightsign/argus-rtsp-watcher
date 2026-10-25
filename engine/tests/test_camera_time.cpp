/**
 * @file test_camera_time.cpp
 * @brief Unit tests for the pure parts of camera capture-time recovery: the
 * NTP -> Unix epoch conversion, the plausibility gate that drops unsynced
 * camera clocks, and the PTS-keyed ring that carries a time from the RTP
 * buffer to the decoded frame (see input/camera_time.h).
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "input/camera_time.h"

namespace {

// 2026-10-25T13:29:53.925Z, computed independently of the code under test.
constexpr int64_t kKnownUnixNs = 1792934993925000000LL;
constexpr uint64_t kKnownNtpNs = 4001923793925000000ULL;

constexpr int64_t kOneSecondNs = camera_time::kNanosecondsPerSecond;

}  // namespace

TEST(CameraTimeNtp, ConvertsAKnownInstantToUnixTime) {
    EXPECT_EQ(camera_time::ntp_ns_to_unix_ns(kKnownNtpNs), kKnownUnixNs);
}

TEST(CameraTimeNtp, TheUnixEpochIsTheNtpOffset) {
    const uint64_t unix_epoch_in_ntp_ns =
        static_cast<uint64_t>(camera_time::kNtpToUnixEpochSeconds) * camera_time::kNanosecondsPerSecond;
    EXPECT_EQ(camera_time::ntp_ns_to_unix_ns(unix_epoch_in_ntp_ns), 0);
}

TEST(CameraTimeNtp, KeepsSubSecondPrecision) {
    EXPECT_EQ(camera_time::ntp_ns_to_unix_ns(kKnownNtpNs + 1), kKnownUnixNs + 1);
}

TEST(CameraTimePlausible, AcceptsTheSameInstant) {
    EXPECT_TRUE(camera_time::is_plausible_source_time(kKnownUnixNs, kKnownUnixNs));
}

TEST(CameraTimePlausible, AcceptsTypicalPipelineLag) {
    // The engine sees the frame ~120 ms after the camera captured it.
    EXPECT_TRUE(camera_time::is_plausible_source_time(kKnownUnixNs, kKnownUnixNs + 120000000LL));
}

TEST(CameraTimePlausible, AcceptsACameraClockSlightlyAhead) {
    // Small negative lag is clock skew, not an error worth hiding.
    EXPECT_TRUE(camera_time::is_plausible_source_time(kKnownUnixNs + 50000000LL, kKnownUnixNs));
}

TEST(CameraTimePlausible, BoundaryIsInclusiveInBothDirections) {
    EXPECT_TRUE(camera_time::is_plausible_source_time(
        kKnownUnixNs, kKnownUnixNs + camera_time::kMaxPlausibleSkewNs));
    EXPECT_TRUE(camera_time::is_plausible_source_time(
        kKnownUnixNs + camera_time::kMaxPlausibleSkewNs, kKnownUnixNs));
    EXPECT_FALSE(camera_time::is_plausible_source_time(
        kKnownUnixNs, kKnownUnixNs + camera_time::kMaxPlausibleSkewNs + 1));
    EXPECT_FALSE(camera_time::is_plausible_source_time(
        kKnownUnixNs + camera_time::kMaxPlausibleSkewNs + 1, kKnownUnixNs));
}

TEST(CameraTimePlausible, RejectsAnUnsyncedCameraClock) {
    // A camera that never set its clock reports 1970-ish time.
    EXPECT_FALSE(camera_time::is_plausible_source_time(5 * kOneSecondNs, kKnownUnixNs));
}

TEST(CameraTimePlausible, RejectsAWrongEpoch) {
    // The NTP value left unconverted is ~70 years ahead of Unix time.
    EXPECT_FALSE(camera_time::is_plausible_source_time(
        static_cast<int64_t>(kKnownNtpNs), kKnownUnixNs));
}

TEST(CameraTimePlausible, ZeroMeansUnavailable) {
    EXPECT_FALSE(camera_time::is_plausible_source_time(0, kKnownUnixNs));
    EXPECT_FALSE(camera_time::is_plausible_source_time(-1, kKnownUnixNs));
}

TEST(CameraTimeRing, MissesWhenNothingWasRecorded) {
    camera_time::ReferenceTimeRing ring;
    EXPECT_FALSE(ring.lookup(1000).has_value());
}

TEST(CameraTimeRing, FindsARecordedPts) {
    camera_time::ReferenceTimeRing ring;
    ring.record(1000, kKnownUnixNs);
    ring.record(2000, kKnownUnixNs + 33000000LL);
    ASSERT_TRUE(ring.lookup(1000).has_value());
    EXPECT_EQ(*ring.lookup(1000), kKnownUnixNs);
    EXPECT_EQ(*ring.lookup(2000), kKnownUnixNs + 33000000LL);
    EXPECT_FALSE(ring.lookup(1500).has_value());
}

TEST(CameraTimeRing, PacketsOfOneFrameConsumeOneSlot) {
    // A frame spans many RTP packets that all carry its PTS; repeating it must
    // not push older frames out of the ring.
    camera_time::ReferenceTimeRing ring;
    ring.record(1, 100);
    for (int packet = 0; packet < 1000; ++packet) {
        ring.record(2, 200);
    }
    EXPECT_TRUE(ring.lookup(1).has_value());
    EXPECT_TRUE(ring.lookup(2).has_value());
}

TEST(CameraTimeRing, OldestEntryIsEvictedAtCapacity) {
    camera_time::ReferenceTimeRing ring;
    const int64_t capacity = static_cast<int64_t>(camera_time::kReferenceRingCapacity);
    for (int64_t pts = 1; pts <= capacity + 1; ++pts) {
        ring.record(pts, pts * 10);
    }
    EXPECT_FALSE(ring.lookup(1).has_value());          // evicted
    EXPECT_TRUE(ring.lookup(2).has_value());           // oldest survivor
    EXPECT_EQ(*ring.lookup(capacity + 1), (capacity + 1) * 10);  // newest
}

TEST(CameraTimeRing, ExactlyAtCapacityKeepsEverything) {
    camera_time::ReferenceTimeRing ring;
    const int64_t capacity = static_cast<int64_t>(camera_time::kReferenceRingCapacity);
    for (int64_t pts = 1; pts <= capacity; ++pts) {
        ring.record(pts, pts * 10);
    }
    for (int64_t pts = 1; pts <= capacity; ++pts) {
        ASSERT_TRUE(ring.lookup(pts).has_value()) << "pts " << pts;
    }
}

TEST(CameraTimeRing, ClearForgetsEverythingAndAcceptsTheSamePtsAgain) {
    // A reconnect restarts the PTS clock, so a previously seen value must be
    // recordable again after clear().
    camera_time::ReferenceTimeRing ring;
    ring.record(5, 50);
    ring.clear();
    EXPECT_FALSE(ring.lookup(5).has_value());
    ring.record(5, 99);
    EXPECT_EQ(*ring.lookup(5), 99);
}

TEST(CameraTimeRing, ConcurrentWriterAndReaderNeverSeeATornEntry) {
    // The streaming thread records while the capture thread looks up. Every
    // value written is pts * 10, so any hit that is not is a torn read.
    camera_time::ReferenceTimeRing ring;
    constexpr int64_t kFrames = 20000;
    std::atomic<bool> done{false};
    std::atomic<int64_t> torn{0};

    std::thread writer([&] {
        for (int64_t pts = 1; pts <= kFrames; ++pts) ring.record(pts, pts * 10);
        done.store(true);
    });
    std::thread reader([&] {
        while (!done.load()) {
            for (int64_t pts = 1; pts <= kFrames; pts += 997) {
                const auto found = ring.lookup(pts);
                if (found && *found != pts * 10) torn.fetch_add(1);
            }
        }
    });
    writer.join();
    reader.join();

    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(*ring.lookup(kFrames), kFrames * 10);
}
