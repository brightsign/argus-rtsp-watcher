#include "analysis/light_json.h"

#include <cinttypes>
#include <cstdio>
#include <algorithm>

namespace {

// Device ids are bounded upstream; the buffer leaves wide margin over the fixed fields.
constexpr size_t kJsonBufferSize = 512;

std::string finish(int written, const char* buffer) {
    // Truncates rather than throws: callers sit behind noexcept publish paths where
    // an exception would terminate the process, matching zone_event/analytics.
    if (written < 0) {
        return std::string();
    }
    const size_t length = std::min(static_cast<size_t>(written), kJsonBufferSize - 1);
    return std::string(buffer, length);
}

}  // namespace

const char* light_state_str(LightLevelState state) noexcept {
    switch (state) {
        case LightLevelState::Light: return "LIGHT";
        case LightLevelState::Dark: return "DARK";
        case LightLevelState::Unknown: break;
    }
    return "UNKNOWN";
}

std::string light_scene_json(const LightState& state) {
    char buffer[kJsonBufferSize];
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\"state\":\"%s\",\"level\":%d,\"ir_mode\":%s,\"luma\":%.1f,\"chroma\":%.1f}",
        light_state_str(state.state), state.level, state.ir_mode ? "true" : "false",
        static_cast<double>(state.luma), static_cast<double>(state.chroma));
    return finish(written, buffer);
}

std::string light_topic_json(const std::string& device_id, const LightState& state,
                             const char* event, int64_t timestamp_ms) {
    // The scene object is identical to the one embedded in the analytics message,
    // so the topic payload wraps that single source of truth rather than
    // re-listing the measurement fields. device_id/event are %s arguments, never
    // the format string, and the scene object is passed as a %s argument too.
    const std::string scene = light_scene_json(state);
    char buffer[kJsonBufferSize];
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\"device_id\":\"%s\",\"event\":\"%s\",\"ts_ms\":%" PRId64 ",\"light\":%s}",
        device_id.c_str(), event, timestamp_ms, scene.c_str());
    return finish(written, buffer);
}

bool light_changed(const LightState& previous, const LightState& current) noexcept {
    return previous.state != current.state || previous.ir_mode != current.ir_mode;
}
