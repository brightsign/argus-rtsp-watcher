#include <gtest/gtest.h>

#include "analysis/light_json.h"

namespace {

LightState makeState(LightLevelState state, int level, bool irMode, float luma, float chroma) {
    LightState result;
    result.state = state;
    result.level = level;
    result.ir_mode = irMode;
    result.luma = luma;
    result.chroma = chroma;
    return result;
}

}  // namespace

TEST(LightJson, StateStrings) {
    EXPECT_STREQ("LIGHT", light_state_str(LightLevelState::Light));
    EXPECT_STREQ("DARK", light_state_str(LightLevelState::Dark));
    EXPECT_STREQ("UNKNOWN", light_state_str(LightLevelState::Unknown));
}

TEST(LightJson, SceneJsonShape) {
    const LightState state = makeState(LightLevelState::Dark, 0, true, 182.44f, 2.06f);
    EXPECT_EQ("{\"state\":\"DARK\",\"level\":0,\"ir_mode\":true,\"luma\":182.4,\"chroma\":2.1}",
              light_scene_json(state));
}

TEST(LightJson, TopicJsonShape) {
    const LightState state = makeState(LightLevelState::Light, 73, false, 120.0f, 40.5f);
    EXPECT_EQ(
        "{\"device_id\":\"cam-01\",\"event\":\"change\",\"state\":\"LIGHT\",\"level\":73,"
        "\"ir_mode\":false,\"luma\":120.0,\"chroma\":40.5,\"ts_ms\":1700000000123}",
        light_topic_json("cam-01", state, "change", 1700000000123LL));
}

TEST(LightJson, ChangedOnStateFlip) {
    const LightState light = makeState(LightLevelState::Light, 80, false, 150.f, 30.f);
    const LightState dark = makeState(LightLevelState::Dark, 5, false, 20.f, 3.f);
    EXPECT_TRUE(light_changed(light, dark));
}

TEST(LightJson, ChangedOnIrFlipSameState) {
    const LightState color = makeState(LightLevelState::Dark, 5, false, 20.f, 30.f);
    const LightState infrared = makeState(LightLevelState::Dark, 0, true, 20.f, 1.f);
    EXPECT_TRUE(light_changed(color, infrared));
}

TEST(LightJson, NotChangedWhenIdentical) {
    const LightState state = makeState(LightLevelState::Light, 80, false, 150.f, 30.f);
    EXPECT_FALSE(light_changed(state, state));
}

TEST(LightJson, OversizedDeviceIdTruncatesInsteadOfThrowing) {
    const std::string hugeDeviceId(2000, 'x');
    const LightState state = makeState(LightLevelState::Light, 80, false, 150.f, 30.f);
    std::string payload;
    ASSERT_NO_THROW(payload = light_topic_json(hugeDeviceId, state, "periodic", 1));
    EXPECT_EQ(511u, payload.size());
}
