/**
 * @file test_frame_sync_json.cpp
 * @brief Unit tests for output::frame_sync_json(), the frame-identification
 * fields of an analytics message: the newest captured frame, the frame the
 * boxes were detected on, and the camera's own capture time for that frame
 * (see output/frame_sync_json.h).
 */

#include <gtest/gtest.h>
#include <3rdparty/nlohmann/json.hpp>
#include "output/frame_sync_json.h"

namespace {

constexpr int64_t kMillisecondNs = 1000000LL;
// 2026-10-25T13:29:53.925Z in ms and ns, computed independently of the code.
constexpr int64_t kCaptureMs = 1792934993925LL;
constexpr int64_t kCaptureNs = kCaptureMs * kMillisecondNs;

// The fields are spliced into a larger object; wrap them to parse as JSON.
nlohmann::json parse(const std::string& fields) {
    return nlohmann::json::parse("{" + fields + "}");
}

}  // namespace

TEST(FrameSyncJson, CarriesBothFramesAndTheCameraTime) {
    // Newest capture is frame 120; the boxes are from frame 117, 100 ms older,
    // and the camera captured that frame 55 ms before the engine saw it.
    const auto j = parse(output::frame_sync_json(
        120, kCaptureNs, 117, kCaptureNs - 100 * kMillisecondNs,
        kCaptureNs - 100 * kMillisecondNs - 55 * kMillisecondNs));

    EXPECT_EQ(j["frame_seq"], 120);
    EXPECT_EQ(j["frame_ts"], kCaptureMs);
    EXPECT_EQ(j["det_seq"], 117);
    EXPECT_EQ(j["det_ts"], kCaptureMs - 100);
    EXPECT_EQ(j["source_ts"], kCaptureMs - 155);
}

TEST(FrameSyncJson, SourceTimeIsNullWhenTheCameraSentNone) {
    const auto j = parse(output::frame_sync_json(5, kCaptureNs, 4, kCaptureNs, 0));
    EXPECT_TRUE(j["source_ts"].is_null());
    EXPECT_EQ(j["det_seq"], 4);
}

TEST(FrameSyncJson, NoDetectionFrameYetReportsZeros) {
    const auto j = parse(output::frame_sync_json(1, kCaptureNs, 0, 0, 0));
    EXPECT_EQ(j["det_seq"], 0);
    EXPECT_EQ(j["det_ts"], 0);
    EXPECT_TRUE(j["source_ts"].is_null());
}

TEST(FrameSyncJson, TruncatesNanosecondsToMilliseconds) {
    const auto j = parse(output::frame_sync_json(1, kCaptureNs + 999999, 1, kCaptureNs + 999999,
                                                 kCaptureNs + 999999));
    EXPECT_EQ(j["frame_ts"], kCaptureMs);
    EXPECT_EQ(j["det_ts"], kCaptureMs);
    EXPECT_EQ(j["source_ts"], kCaptureMs);
}

TEST(FrameSyncJson, LargeSequenceNumbersSurviveTheFormat) {
    const auto j = parse(output::frame_sync_json(UINT64_MAX, kCaptureNs, UINT64_MAX, kCaptureNs, 0));
    EXPECT_EQ(j["frame_seq"].get<uint64_t>(), UINT64_MAX);
    EXPECT_EQ(j["det_seq"].get<uint64_t>(), UINT64_MAX);
}

TEST(FrameSyncJson, HasNoBracesOrTrailingComma) {
    // The caller supplies the surrounding object and the comma that follows.
    const std::string fields = output::frame_sync_json(1, kCaptureNs, 1, kCaptureNs, 0);
    EXPECT_EQ(fields.find('{'), std::string::npos);
    EXPECT_EQ(fields.find('}'), std::string::npos);
    EXPECT_NE(fields.back(), ',');
}
