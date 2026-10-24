#ifndef LIGHT_JSON_H
#define LIGHT_JSON_H

#include <cstdint>
#include <string>

#include "analysis/light_state.h"

const char* light_state_str(LightLevelState state) noexcept;

/// JSON object for the scene summary: state, level, ir_mode, luma, chroma.
std::string light_scene_json(const LightState& state);

/// Full payload for the per-device `light` MQTT topic. `event` must be
/// "periodic" or "change".
std::string light_topic_json(const std::string& device_id, const LightState& state,
                             const char* event, int64_t timestamp_ms);

/// True when a consumer-visible field (state or IR mode) differs.
bool light_changed(const LightState& previous, const LightState& current) noexcept;

#endif  // LIGHT_JSON_H
