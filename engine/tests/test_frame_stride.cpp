/**
 * @file test_frame_stride.cpp
 * @brief Unit tests for output::FrameStride, which picks the processed frames
 * the preview writer saves (see output/frame_stride.h).
 */

#include <gtest/gtest.h>
#include <vector>
#include "output/frame_stride.h"

using output::FrameStride;

namespace {

std::vector<bool> run(int stride, int frames) {
    FrameStride picker(stride);
    std::vector<bool> saved;
    for (int i = 0; i < frames; ++i) saved.push_back(picker.admit());
    return saved;
}

}  // namespace

TEST(FrameStride, StrideOneSavesEveryFrame) {
    for (const bool saved : run(1, 10)) EXPECT_TRUE(saved);
}

TEST(FrameStride, StrideTwoSavesEveryOtherFrameStartingWithTheFirst) {
    EXPECT_EQ(run(2, 6), (std::vector<bool>{true, false, true, false, true, false}));
}

TEST(FrameStride, StrideThreeMatchesTheOldHardCodedBehaviour) {
    EXPECT_EQ(run(3, 7), (std::vector<bool>{true, false, false, true, false, false, true}));
}

TEST(FrameStride, TheFirstFrameIsAlwaysSaved) {
    // A fresh preview should not wait `stride` frames for its first image.
    for (const int stride : {1, 2, 3, 10, 100}) {
        EXPECT_TRUE(run(stride, 1)[0]) << "stride " << stride;
    }
}

TEST(FrameStride, SavesTheRightNumberOverALongRun) {
    int saved = 0;
    FrameStride picker(4);
    for (int i = 0; i < 1000; ++i) saved += picker.admit() ? 1 : 0;
    EXPECT_EQ(saved, 250);
}
