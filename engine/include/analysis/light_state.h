#ifndef LIGHT_STATE_H
#define LIGHT_STATE_H

#include <cstdint>

enum class LightLevelState : uint8_t { Unknown = 0, Light = 1, Dark = 2 };

/// Snapshot of the ambient-light decision published to consumers.
struct LightState {
    LightLevelState state{LightLevelState::Unknown};
    int level{0};          ///< 0..100 real-world light; 0 in IR mode
    bool ir_mode{false};   ///< camera is in monochrome/IR night mode
    float luma{0.f};       ///< smoothed BT.601 luma, 0..255
    float chroma{0.f};     ///< smoothed max(R,G,B)-min(R,G,B), 0..255
    int64_t last_change_ms{0};
};

#endif  // LIGHT_STATE_H
