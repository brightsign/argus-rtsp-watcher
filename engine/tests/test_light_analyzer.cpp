/**
 * @file test_light_analyzer.cpp
 * @brief Unit tests for LightAnalyzer: BT.601 luma / chroma measurement and the
 * UNKNOWN/LIGHT/DARK state machine (spec 2026-10-24-light-dark-detection-design.md).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "analysis/light_analyzer.h"
#include "pipeline/shared_frame.h"

namespace {

constexpr int64_t kMillisPerSecond = 1000;
constexpr int kFrameWidth = 64;
constexpr int kFrameHeight = 48;

SharedFrame make_bgr(int width, int height, uint8_t blue, uint8_t green, uint8_t red) {
    SharedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.bgr.resize(static_cast<size_t>(width) * height * 3);
    for (size_t offset = 0; offset < frame.bgr.size(); offset += 3) {
        frame.bgr[offset] = blue;
        frame.bgr[offset + 1] = green;
        frame.bgr[offset + 2] = red;
    }
    return frame;
}

// alpha 1.0 makes the EMA equal the latest sample so tests are deterministic.
LightCfg fast_cfg() {
    LightCfg cfg;
    cfg.ema_alpha = 1.0f;
    return cfg;
}

// Feeds one frame per second for the given number of seconds; returns the next now_ms.
int64_t feed(LightAnalyzer& analyzer, const SharedFrame& frame, int64_t start_ms, int seconds) {
    int64_t now_ms = start_ms;
    for (int step = 0; step < seconds; ++step) {
        analyzer.update(frame, now_ms);
        now_ms += kMillisPerSecond;
    }
    return now_ms;
}

float luma_of(uint8_t blue, uint8_t green, uint8_t red) {
    return 0.299f * red + 0.587f * green + 0.114f * blue;
}

// Overwrites the pixel rectangle [x0,x1) x [y0,y1) with a BGR colour.
void fill_rect(SharedFrame& frame, int x0, int y0, int x1, int y1,
               uint8_t blue, uint8_t green, uint8_t red) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint8_t* pixel = frame.bgr.data() + (static_cast<size_t>(y) * frame.width + x) * 3;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
        }
    }
}

// A fast-EMA config whose decision keys on an operator-drawn region.
LightCfg region_cfg(float x0, float y0, float x1, float y1, float threshold) {
    LightCfg cfg = fast_cfg();
    cfg.region_enabled = true;
    cfg.region_x0 = x0;
    cfg.region_y0 = y0;
    cfg.region_x1 = x1;
    cfg.region_y1 = y1;
    cfg.region_dark_luma = threshold;
    return cfg;
}

}  // namespace

TEST(LightAnalyzer, BrightColorIsLight) {
    LightAnalyzer analyzer(fast_cfg());
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Light);
    EXPECT_FALSE(state.ir_mode);
    EXPECT_EQ(state.level, static_cast<int>(std::lround(100.0f * luma_of(60, 160, 200) / 255.0f)));
}

TEST(LightAnalyzer, DarkColorIsDark) {
    LightAnalyzer analyzer(fast_cfg());
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 5, 30, 50);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Dark);
    EXPECT_FALSE(state.ir_mode);
}

TEST(LightAnalyzer, BrightMonochromeIsDarkViaIR) {
    LightAnalyzer analyzer(fast_cfg());
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 200, 200, 200);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Dark);
    EXPECT_TRUE(state.ir_mode);
    EXPECT_EQ(state.level, 0);
}

TEST(LightAnalyzer, BrightMonochromeIsLightWhenMonoCamera) {
    LightCfg cfg = fast_cfg();
    cfg.monochrome_camera = true;
    LightAnalyzer analyzer(cfg);
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 200, 200, 200);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Light);
    EXPECT_FALSE(state.ir_mode);
}

TEST(LightAnalyzer, DeadBandHoldsState) {
    LightAnalyzer analyzer(fast_cfg());
    int64_t now_ms = feed(analyzer, make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0, 10);
    ASSERT_EQ(analyzer.snapshot().state, LightLevelState::Light);

    // Luma ~55 lies strictly between dark_luma (40) and light_luma (70), with chroma.
    const SharedFrame middling = make_bgr(kFrameWidth, kFrameHeight, 30, 60, 80);
    const float middling_luma = luma_of(30, 60, 80);
    ASSERT_GT(middling_luma, 40.0f);
    ASSERT_LT(middling_luma, 70.0f);
    feed(analyzer, middling, now_ms, 30);

    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Light);
}

TEST(LightAnalyzer, DwellSuppressesRapidFlip) {
    LightAnalyzer analyzer(fast_cfg());
    feed(analyzer, make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0, 10);
    ASSERT_EQ(analyzer.snapshot().state, LightLevelState::Light);
    const int64_t committed_ms = analyzer.snapshot().last_change_ms;

    const SharedFrame dark = make_bgr(kFrameWidth, kFrameHeight, 5, 30, 50);
    // Well under min_dwell_seconds (5s) after the Light commit.
    analyzer.update(dark, committed_ms + 1 * kMillisPerSecond);
    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Light);

    analyzer.update(dark, committed_ms + 6 * kMillisPerSecond);
    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Dark);
}

TEST(LightAnalyzer, WarmupIsUnknown) {
    LightAnalyzer analyzer(fast_cfg());
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200);
    // Samples spanning 2s of a 3s warmup.
    analyzer.update(frame, 0);
    analyzer.update(frame, 1 * kMillisPerSecond);
    analyzer.update(frame, 2 * kMillisPerSecond);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Unknown);
    EXPECT_FALSE(state.ir_mode);
    EXPECT_EQ(state.level, 0);
    EXPECT_GT(state.luma, 0.0f);
}

TEST(LightAnalyzer, EmptyFrameIsSafe) {
    LightAnalyzer analyzer(fast_cfg());
    SharedFrame frame;
    frame.width = kFrameWidth;
    frame.height = kFrameHeight;
    analyzer.update(frame, 0);
    analyzer.update(frame, 100 * kMillisPerSecond);

    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Unknown);
}

TEST(LightAnalyzer, TinyFrameIsSafe) {
    LightCfg cfg = fast_cfg();
    cfg.sample_stride = 8;
    LightAnalyzer analyzer(cfg);
    const SharedFrame frame = make_bgr(2, 2, 60, 160, 200);
    analyzer.update(frame, 0);

    const LightState state = analyzer.snapshot();
    EXPECT_NEAR(state.luma, luma_of(60, 160, 200), 1.0f);
    EXPECT_NEAR(state.chroma, 140.0f, 1.0f);
}

TEST(LightAnalyzer, FirstCommitRecordsChangeTimestamp) {
    LightAnalyzer analyzer(fast_cfg());
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200);
    const int64_t start_ms = 50 * kMillisPerSecond;
    const int64_t warmup_end_ms = start_ms + 3 * kMillisPerSecond;

    analyzer.update(frame, start_ms);
    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Unknown);
    analyzer.update(frame, warmup_end_ms);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Light);
    EXPECT_EQ(state.last_change_ms, warmup_end_ms);
}

TEST(LightAnalyzer, IrExitIntoDeadBandHoldsDarkInColourMode) {
    LightAnalyzer analyzer(fast_cfg());
    int64_t now_ms = feed(analyzer, make_bgr(kFrameWidth, kFrameHeight, 200, 200, 200), 0, 10);
    ASSERT_EQ(analyzer.snapshot().state, LightLevelState::Dark);
    ASSERT_TRUE(analyzer.snapshot().ir_mode);

    // Chroma 50 (recovered), luma between dark_luma (40) and light_luma (70).
    const float middling_luma = luma_of(30, 60, 80);
    ASSERT_GT(middling_luma, 40.0f);
    ASSERT_LT(middling_luma, 70.0f);
    feed(analyzer, make_bgr(kFrameWidth, kFrameHeight, 30, 60, 80), now_ms, 30);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Dark);
    EXPECT_FALSE(state.ir_mode);
    EXPECT_GT(state.level, 0);
}

TEST(LightAnalyzer, ZeroStrideIsClampedAndSamples) {
    LightCfg cfg = fast_cfg();
    cfg.sample_stride = 0;
    LightAnalyzer analyzer(cfg);
    analyzer.update(make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0);
    EXPECT_NEAR(analyzer.snapshot().luma, luma_of(60, 160, 200), 1.0f);
}

TEST(LightAnalyzer, ZeroRoiFractionIsClampedAndSamples) {
    LightCfg cfg = fast_cfg();
    cfg.roi_frac = 0.0f;
    LightAnalyzer analyzer(cfg);
    analyzer.update(make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0);
    EXPECT_NEAR(analyzer.snapshot().luma, luma_of(60, 160, 200), 1.0f);
}

TEST(LightAnalyzer, OversizedRoiFractionIsClampedAndSamples) {
    LightCfg cfg = fast_cfg();
    cfg.roi_frac = 5.0f;
    LightAnalyzer analyzer(cfg);
    analyzer.update(make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0);
    EXPECT_NEAR(analyzer.snapshot().luma, luma_of(60, 160, 200), 1.0f);
}

TEST(LightAnalyzer, NanRoiFractionAndOutOfRangeAlphaAreSafe) {
    LightCfg cfg;
    cfg.roi_frac = std::nanf("");
    cfg.ema_alpha = 7.0f;
    LightAnalyzer analyzer(cfg);
    analyzer.update(make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200), 0);
    EXPECT_NEAR(analyzer.snapshot().luma, luma_of(60, 160, 200), 1.0f);
}

TEST(LightAnalyzer, ZeroWidthFrameIsSafe) {
    LightAnalyzer analyzer(fast_cfg());
    SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200);
    frame.width = 0;
    analyzer.update(frame, 0);
    analyzer.update(frame, 100 * kMillisPerSecond);
    EXPECT_EQ(analyzer.snapshot().state, LightLevelState::Unknown);
}

// A bright monochrome frame would be DARK via the IR/chroma rule, but a region
// above its threshold overrides that: the region is the sole authority.
TEST(LightAnalyzer, RegionBypassesIrMode) {
    LightAnalyzer analyzer(region_cfg(0.1f, 0.1f, 0.9f, 0.9f, 40.0f));
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 200, 200, 200);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Light);
    EXPECT_FALSE(state.ir_mode);
    EXPECT_GT(state.level, 0);
}

// A mid-luma colour frame sits in the central detector's dead-band (stays
// UNKNOWN), but a region with a high threshold reads it DARK -- proving the
// region threshold, not the dark_luma/light_luma band, governs the decision.
TEST(LightAnalyzer, RegionThresholdIsAuthority) {
    LightAnalyzer analyzer(region_cfg(0.1f, 0.1f, 0.9f, 0.9f, 100.0f));
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 30, 60, 80);
    ASSERT_GT(luma_of(30, 60, 80), 40.0f);
    ASSERT_LT(luma_of(30, 60, 80), 70.0f);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Dark);
    EXPECT_FALSE(state.ir_mode);
}

// The region samples only the drawn rectangle: a dark patch under the region
// reads DARK, while a region over the bright remainder reads LIGHT.
TEST(LightAnalyzer, RegionSamplesOnlyItsRectangle) {
    SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 60, 160, 200);
    fill_rect(frame, 0, 0, kFrameWidth / 2, kFrameHeight / 2, 5, 5, 5);  // dark top-left quadrant

    LightAnalyzer over_dark(region_cfg(0.0f, 0.0f, 0.5f, 0.5f, 40.0f));
    feed(over_dark, frame, 0, 10);
    EXPECT_EQ(over_dark.snapshot().state, LightLevelState::Dark);

    LightAnalyzer over_bright(region_cfg(0.5f, 0.5f, 1.0f, 1.0f, 40.0f));
    feed(over_bright, frame, 0, 10);
    EXPECT_EQ(over_bright.snapshot().state, LightLevelState::Light);
}

// A region with non-increasing coordinates is dropped; the analyzer falls back
// to the central-ROI IR/chroma detector.
TEST(LightAnalyzer, DegenerateRegionFallsBackToCentral) {
    LightAnalyzer analyzer(region_cfg(0.5f, 0.1f, 0.5f, 0.9f, 40.0f));
    const SharedFrame frame = make_bgr(kFrameWidth, kFrameHeight, 200, 200, 200);
    feed(analyzer, frame, 0, 10);

    const LightState state = analyzer.snapshot();
    EXPECT_EQ(state.state, LightLevelState::Dark);
    EXPECT_TRUE(state.ir_mode);
}
