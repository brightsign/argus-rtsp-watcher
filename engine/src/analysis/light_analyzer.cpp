#include "analysis/light_analyzer.h"

#include <algorithm>
#include <cmath>

namespace {

// Keeps per-frame cost roughly constant across camera resolutions.
constexpr double kTargetSampleCount = 2048.0;
constexpr float kMinimumRoiFraction = 0.01f;
constexpr int kMaxLevel = 100;
constexpr float kMaxChannelValue = 255.f;
constexpr int64_t kMillisPerSecond = 1000;

// BT.601 luma weights scaled by 1000 so the hot loop stays in integers.
constexpr uint32_t kLumaWeightRed = 299;
constexpr uint32_t kLumaWeightGreen = 587;
constexpr uint32_t kLumaWeightBlue = 114;
constexpr double kLumaWeightScale = 1000.0;

struct Measurement {
    float luma;
    float chroma;
};

Measurement measure(const SharedFrame& sf, float roi_frac, int minimum_stride) {
    const int roi_width = std::max(1, static_cast<int>(sf.width * roi_frac));
    const int roi_height = std::max(1, static_cast<int>(sf.height * roi_frac));
    const int roi_left = (sf.width - roi_width) / 2;
    const int roi_top = (sf.height - roi_height) / 2;

    const double roi_pixels = static_cast<double>(roi_width) * roi_height;
    const int derived_stride = static_cast<int>(std::lround(std::sqrt(roi_pixels / kTargetSampleCount)));
    const int stride = std::max(minimum_stride, std::max(1, derived_stride));

    // 64-bit sums: at 1080p a 32-bit accumulator could overflow.
    uint64_t luma_sum = 0;
    uint64_t chroma_sum = 0;
    uint64_t sample_count = 0;

    for (int row = roi_top; row < roi_top + roi_height; row += stride) {
        const uint8_t* row_pixels = sf.bgr.data() + static_cast<size_t>(row) * sf.width * 3;
        for (int column = roi_left; column < roi_left + roi_width; column += stride) {
            const uint8_t* pixel = row_pixels + static_cast<size_t>(column) * 3;
            const uint32_t blue = pixel[0];
            const uint32_t green = pixel[1];
            const uint32_t red = pixel[2];
            luma_sum += kLumaWeightRed * red + kLumaWeightGreen * green + kLumaWeightBlue * blue;
            chroma_sum += std::max({red, green, blue}) - std::min({red, green, blue});
            ++sample_count;
        }
    }

    const double count = static_cast<double>(sample_count);
    return {static_cast<float>(static_cast<double>(luma_sum) / kLumaWeightScale / count),
            static_cast<float>(static_cast<double>(chroma_sum) / count)};
}

}  // namespace

LightAnalyzer::LightAnalyzer(LightCfg cfg) noexcept : cfg_(cfg) {
    cfg_.sample_stride = std::max(1, cfg_.sample_stride);
    cfg_.roi_frac = std::clamp(cfg_.roi_frac, kMinimumRoiFraction, 1.0f);
}

void LightAnalyzer::update(const SharedFrame& sf, int64_t now_ms) noexcept {
    if (sf.width <= 0 || sf.height <= 0 ||
        sf.bgr.size() < static_cast<size_t>(sf.width) * sf.height * 3) {
        return;
    }

    const Measurement sample = measure(sf, cfg_.roi_frac, cfg_.sample_stride);

    std::lock_guard<std::mutex> lock(mutex_);

    if (!seeded_) {
        seeded_ = true;
        first_sample_ms_ = now_ms;
        ema_luma_ = sample.luma;
        ema_chroma_ = sample.chroma;
    } else {
        ema_luma_ += cfg_.ema_alpha * (sample.luma - ema_luma_);
        ema_chroma_ += cfg_.ema_alpha * (sample.chroma - ema_chroma_);
    }
    current_.luma = ema_luma_;
    current_.chroma = ema_chroma_;

    const bool warmed_up = now_ms - first_sample_ms_ >= cfg_.warmup_seconds * kMillisPerSecond;
    if (!warmed_up) {
        return;
    }

    LightLevelState candidate_state = current_.state;
    bool candidate_ir_mode = false;
    if (!cfg_.monochrome_camera && ema_chroma_ < cfg_.chroma_threshold) {
        candidate_state = LightLevelState::Dark;
        candidate_ir_mode = true;
    } else if (ema_luma_ <= cfg_.dark_luma) {
        candidate_state = LightLevelState::Dark;
    } else if (ema_luma_ >= cfg_.light_luma) {
        candidate_state = LightLevelState::Light;
    }

    const bool changed = candidate_state != current_.state || candidate_ir_mode != current_.ir_mode;
    const bool first_commit = current_.state == LightLevelState::Unknown;
    const bool dwell_elapsed =
        now_ms - current_.last_change_ms >= cfg_.min_dwell_seconds * kMillisPerSecond;

    if (changed && (first_commit || dwell_elapsed)) {
        current_.state = candidate_state;
        current_.ir_mode = candidate_ir_mode;
        current_.last_change_ms = now_ms;
    }

    if (current_.state == LightLevelState::Unknown || current_.ir_mode) {
        current_.level = 0;
    } else {
        const long level = std::lround(100.0 * ema_luma_ / kMaxChannelValue);
        current_.level = static_cast<int>(std::clamp<long>(level, 0, kMaxLevel));
    }
}

LightState LightAnalyzer::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}
