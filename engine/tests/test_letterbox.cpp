/**
 * @file test_letterbox.cpp
 * @brief Unit tests for letterbox::compute(), the geometry shared by the NPU
 * input resize and every de-letterbox that maps detections back to camera
 * pixels (see orchestration/letterbox.h). The resize and the inverse mapping
 * must agree to the pixel or boxes drift off the people they mark.
 */

#include <gtest/gtest.h>
#include "orchestration/letterbox.h"

namespace {

constexpr float kPixelTolerance = 1.0e-3f;

}  // namespace

TEST(Letterbox, FourByThreeCameraIntoSquareModel) {
    // 2560x1920 -> 640x640: scale 0.25, content 640x480, bars top and bottom.
    const auto g = letterbox::compute(2560, 1920, 640, 640);
    EXPECT_FLOAT_EQ(g.scale, 0.25f);
    EXPECT_EQ(g.content_w, 640);
    EXPECT_EQ(g.content_h, 480);
    EXPECT_EQ(g.pad_x, 0);
    EXPECT_EQ(g.pad_y, 80);
}

TEST(Letterbox, SixteenByNineCameraIntoSquareModel) {
    // 2560x1440 -> 640x640: scale 0.25, content 640x360, bars of 140px.
    const auto g = letterbox::compute(2560, 1440, 640, 640);
    EXPECT_FLOAT_EQ(g.scale, 0.25f);
    EXPECT_EQ(g.content_w, 640);
    EXPECT_EQ(g.content_h, 360);
    EXPECT_EQ(g.pad_x, 0);
    EXPECT_EQ(g.pad_y, 140);
}

TEST(Letterbox, FullHdIntoFaceModel) {
    // 1920x1080 -> 320x320: content 320x180, bars of 70px.
    const auto g = letterbox::compute(1920, 1080, 320, 320);
    EXPECT_EQ(g.content_w, 320);
    EXPECT_EQ(g.content_h, 180);
    EXPECT_EQ(g.pad_x, 0);
    EXPECT_EQ(g.pad_y, 70);
}

TEST(Letterbox, PortraitCameraPadsLeftAndRight) {
    // 1080x1920 -> 640x640: the height binds, so the bars are on the sides.
    const auto g = letterbox::compute(1080, 1920, 640, 640);
    EXPECT_EQ(g.content_w, 360);
    EXPECT_EQ(g.content_h, 640);
    EXPECT_EQ(g.pad_x, 140);
    EXPECT_EQ(g.pad_y, 0);
}

TEST(Letterbox, SameSizeIsIdentity) {
    const auto g = letterbox::compute(640, 480, 640, 480);
    EXPECT_FLOAT_EQ(g.scale, 1.0f);
    EXPECT_EQ(g.content_w, 640);
    EXPECT_EQ(g.content_h, 480);
    EXPECT_EQ(g.pad_x, 0);
    EXPECT_EQ(g.pad_y, 0);
}

TEST(Letterbox, ContentAndPaddingTruncateRatherThanRound) {
    // 4x3 into 10x10: scale 2.5, content_h 7.5 -> 7, pad_y (10-7)/2 = 1.5 -> 1.
    // The engine has always truncated; changing it would shift every box.
    const auto g = letterbox::compute(4, 3, 10, 10);
    EXPECT_FLOAT_EQ(g.scale, 2.5f);
    EXPECT_EQ(g.content_w, 10);
    EXPECT_EQ(g.content_h, 7);
    EXPECT_EQ(g.pad_x, 0);
    EXPECT_EQ(g.pad_y, 1);
}

TEST(Letterbox, CenterOfModelMapsToCenterOfCamera) {
    // The model center is the camera center regardless of the bars.
    const auto g = letterbox::compute(2560, 1920, 640, 640);
    EXPECT_NEAR(g.to_source_x(320.0f), 1280.0f, kPixelTolerance);
    EXPECT_NEAR(g.to_source_y(320.0f), 960.0f, kPixelTolerance);
}

TEST(Letterbox, BarEdgeMapsToCameraOrigin) {
    // The top-left of the content, not of the bars, is camera pixel (0,0).
    const auto g = letterbox::compute(2560, 1440, 640, 640);
    EXPECT_NEAR(g.to_source_x(static_cast<float>(g.pad_x)), 0.0f, kPixelTolerance);
    EXPECT_NEAR(g.to_source_y(static_cast<float>(g.pad_y)), 0.0f, kPixelTolerance);
}

TEST(Letterbox, DestToSourceInvertsSourceToDest) {
    const auto g = letterbox::compute(1920, 1080, 640, 640);
    for (const float source_x : {0.0f, 1.0f, 777.0f, 1919.0f}) {
        EXPECT_NEAR(g.to_source_x(g.to_dest_x(source_x)), source_x, kPixelTolerance);
    }
    for (const float source_y : {0.0f, 1.0f, 333.0f, 1079.0f}) {
        EXPECT_NEAR(g.to_source_y(g.to_dest_y(source_y)), source_y, kPixelTolerance);
    }
}

TEST(Letterbox, ContentAlwaysFitsInsideDestination) {
    // Property sweep over the camera sizes this fleet actually sees and both
    // model input sizes: content never overflows and the bars are never
    // negative or lopsided by more than the one truncated pixel.
    const int cameras[][2] = {{2560, 1920}, {2560, 1440}, {1920, 1080}, {1280, 720},
                              {640, 480},   {1080, 1920}, {1000, 333}};
    for (const auto& camera : cameras) {
        for (const int model : {320, 640}) {
            const auto g = letterbox::compute(camera[0], camera[1], model, model);
            EXPECT_GT(g.scale, 0.0f);
            EXPECT_GT(g.content_w, 0);
            EXPECT_GT(g.content_h, 0);
            EXPECT_LE(g.content_w, model);
            EXPECT_LE(g.content_h, model);
            EXPECT_GE(g.pad_x, 0);
            EXPECT_GE(g.pad_y, 0);
            EXPECT_LE(g.pad_x * 2 + g.content_w, model);
            EXPECT_LE(g.pad_y * 2 + g.content_h, model);
            EXPECT_LE(model - (g.pad_x * 2 + g.content_w), 1);
            EXPECT_LE(model - (g.pad_y * 2 + g.content_h), 1);
        }
    }
}
