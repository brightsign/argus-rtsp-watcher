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
