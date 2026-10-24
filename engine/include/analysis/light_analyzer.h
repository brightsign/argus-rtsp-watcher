#ifndef LIGHT_ANALYZER_H
#define LIGHT_ANALYZER_H

#include <cstdint>
#include <mutex>

#include "analysis/light_config.h"
#include "analysis/light_state.h"
#include "pipeline/shared_frame.h"

/**
 * Measures ambient light from camera frames and decides LIGHT/DARK with IR
 * awareness. update() runs on the capture thread; snapshot() may be called
 * from any other thread.
 */
class LightAnalyzer {
public:
    explicit LightAnalyzer(LightCfg cfg) noexcept;

    void update(const SharedFrame& sf, int64_t now_ms) noexcept;
    LightState snapshot() const noexcept;

private:
    LightCfg cfg_;
    mutable std::mutex mutex_;
    LightState current_;
    bool seeded_{false};
    int64_t first_sample_ms_{0};
    float ema_luma_{0.f};
    float ema_chroma_{0.f};
};

#endif  // LIGHT_ANALYZER_H
