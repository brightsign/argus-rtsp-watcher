/**
 * @file test_publishers.cpp
 * @brief Publisher Tests (ported from argus; trimmed to MQTT-only)
 *
 * Tests for publisher configuration and message formatting.
 *
 * Trimmed from the upstream argus test suite: this project is MQTT-only for
 * v1 (no Prometheus, no UDP/file/BrightSignV3 publishers - see
 * global-constraints.md). The UdpPublisher/FilePublisher-specific test cases
 * (UdpPublisherDefaults, UdpPublisherCustom, FilePublisherDefaults,
 * FilePublisherCustom) were removed because those publisher backends are
 * dropped features. PublisherConfigKindSelection was trimmed to its MQTT
 * case for the same reason. The UdpPublisher/FilePublisher config structs
 * still exist in publisher_config.h (retained for config parsing), so the
 * PublisherKind enum test and PublisherConfigDefaults test - which only
 * check that those structs still default-construct correctly - are kept
 * unmodified.
 *
 * Note: These are pure unit tests that test configuration structures and
 * message format validation without requiring actual network/file I/O.
 *
 * Test Suite: PublisherTest
 */

#include <gtest/gtest.h>
#include "config/publisher_config.h"
#include "config/config_common.h"
#include <string>
#include <cstring>

// ============================================================================
// Test Fixture
// ============================================================================

class PublisherTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup before each test
    }

    void TearDown() override {
        // Cleanup after each test
    }
};

// ============================================================================
// TC-050: Publisher Kind Enum
// ============================================================================

TEST_F(PublisherTest, PublisherKindEnum) {
    // Test PublisherKind enum values
    PublisherKind udp = PublisherKind::UDP;
    PublisherKind file = PublisherKind::File;
    PublisherKind mqtt = PublisherKind::Mqtt;
    PublisherKind stdout_kind = PublisherKind::Stdout;
    PublisherKind none = PublisherKind::None;

    // Enums should be distinct
    EXPECT_NE(udp, file);
    EXPECT_NE(udp, mqtt);
    EXPECT_NE(udp, stdout_kind);
    EXPECT_NE(udp, none);
    EXPECT_NE(mqtt, file);
}

// ============================================================================
// TC-053: MQTT Publisher Configuration
// ============================================================================

TEST_F(PublisherTest, MqttPublisherDefaults) {
    // Test MQTT publisher defaults
    MqttPublisherConfig mqtt;

    EXPECT_EQ(mqtt.host, "127.0.0.1") << "Default MQTT broker host";
    EXPECT_EQ(mqtt.port, 1883) << "Default MQTT port";
    EXPECT_EQ(mqtt.client_id, "xt5-gaze") << "Default client ID";
    EXPECT_EQ(mqtt.topic, "bs/argus/analytics") << "Default topic";
    EXPECT_EQ(mqtt.qos, 1) << "Default QoS level 1";
    EXPECT_FALSE(mqtt.retain) << "Default should not retain messages";
    EXPECT_EQ(mqtt.period_ms, 1000) << "Default publish period 1000ms";
    EXPECT_TRUE(mqtt.username.empty()) << "Default no username";
    EXPECT_TRUE(mqtt.password.empty()) << "Default no password";
    EXPECT_TRUE(mqtt.clean_session) << "Default clean session enabled";
}

TEST_F(PublisherTest, MqttPublisherCustom) {
    // Test custom MQTT configuration
    MqttPublisherConfig mqtt;
    mqtt.host = "mqtt.example.com";
    mqtt.port = 8883;  // TLS port
    mqtt.client_id = "device-123";
    mqtt.topic = "custom/analytics";
    mqtt.qos = 2;
    mqtt.retain = true;
    mqtt.period_ms = 500;
    mqtt.username = "user";
    mqtt.password = "pass";
    mqtt.clean_session = false;

    EXPECT_EQ(mqtt.host, "mqtt.example.com");
    EXPECT_EQ(mqtt.port, 8883);
    EXPECT_EQ(mqtt.client_id, "device-123");
    EXPECT_EQ(mqtt.topic, "custom/analytics");
    EXPECT_EQ(mqtt.qos, 2);
    EXPECT_TRUE(mqtt.retain);
    EXPECT_EQ(mqtt.period_ms, 500);
    EXPECT_EQ(mqtt.username, "user");
    EXPECT_EQ(mqtt.password, "pass");
    EXPECT_FALSE(mqtt.clean_session);
}

// ============================================================================
// TC-054: Publisher Config Defaults
// ============================================================================

TEST_F(PublisherTest, PublisherConfigDefaults) {
    // Test PublisherConfig defaults
    PublisherConfig config;

    EXPECT_EQ(config.kind, PublisherKind::Stdout) << "Default publisher is Stdout";

    // Check nested defaults are initialized
    EXPECT_EQ(config.udp.host, "127.0.0.1");
    EXPECT_EQ(config.file.dir, "/storage/sd/out");
    EXPECT_EQ(config.mqtt.host, "127.0.0.1");
}

TEST_F(PublisherTest, PublisherConfigKindSelection) {
    // Test MQTT publisher kind selection (the only publisher shipped in v1)
    PublisherConfig config_mqtt;
    config_mqtt.kind = PublisherKind::Mqtt;
    config_mqtt.mqtt.host = "broker.local";

    EXPECT_EQ(config_mqtt.kind, PublisherKind::Mqtt);
    EXPECT_EQ(config_mqtt.mqtt.host, "broker.local");
}

// ============================================================================
// TC-055: MQTT Message Schema Version
// ============================================================================

TEST_F(PublisherTest, MqttMessageSchemaVersion) {
    // Test MQTT message schema version format
    std::string schema_v7 = "analytics/v7.0";
    std::string schema_v6 = "analytics/v6.2";

    // Schema format should be "analytics/vX.Y"
    EXPECT_NE(schema_v7.find("analytics/v"), std::string::npos)
        << "Schema should contain 'analytics/v' prefix";
    EXPECT_NE(schema_v6.find("analytics/v"), std::string::npos);

    // Version numbers should be extractable
    size_t v_pos = schema_v7.find('v');
    EXPECT_NE(v_pos, std::string::npos);
    std::string version = schema_v7.substr(v_pos + 1);
    EXPECT_EQ(version, "7.0");
}

// ============================================================================
// TC-056: MQTT Track State Enum Values
// ============================================================================

TEST_F(PublisherTest, MqttTrackStateValues) {
    // Test that track state values match MQTT schema
    // Schema allows: "Tentative", "Confirmed", "Lost"

    std::vector<std::string> valid_states = {
        "Tentative",
        "Confirmed",
        "Lost"
    };

    // These should be the only valid state strings in MQTT messages
    EXPECT_EQ(valid_states.size(), 3);
    EXPECT_EQ(valid_states[0], "Tentative");
    EXPECT_EQ(valid_states[1], "Confirmed");
    EXPECT_EQ(valid_states[2], "Lost");
}

// ============================================================================
// TC-057: MQTT Direction Enum Values
// ============================================================================

TEST_F(PublisherTest, MqttDirectionValues) {
    // Test direction values match MQTT schema
    // Schema allows: "R", "UR", "U", "UL", "L", "DL", "D", "DR", "?"

    std::vector<std::string> valid_directions = {
        "R",   // Right (0°)
        "UR",  // Up-Right (45°)
        "U",   // Up (90°)
        "UL",  // Up-Left (135°)
        "L",   // Left (180°)
        "DL",  // Down-Left (225°)
        "D",   // Down (270°)
        "DR",  // Down-Right (315°)
        "?"    // Unknown/Stationary
    };

    EXPECT_EQ(valid_directions.size(), 9);

    // Test each direction string
    for (const auto& dir : valid_directions) {
        EXPECT_LE(dir.length(), 2) << "Direction strings should be 1-2 chars";
    }

    // Test stationary/unknown
    EXPECT_EQ(valid_directions[8], "?");
}

// ============================================================================
// TC-058: MQTT BBox Format Validation
// ============================================================================

TEST_F(PublisherTest, MqttBBoxFormat) {
    // Test bounding box format: [x0, y0, x1, y1]
    // Must be 4 floats: top-left (x0,y0), bottom-right (x1,y1)

    struct BBox {
        float x0, y0, x1, y1;
    };

    BBox bbox = {100.5f, 200.3f, 300.7f, 400.9f};

    // Validate format
    EXPECT_LT(bbox.x0, bbox.x1) << "x0 must be less than x1";
    EXPECT_LT(bbox.y0, bbox.y1) << "y0 must be less than y1";

    // Validate positive coordinates
    EXPECT_GE(bbox.x0, 0.0f);
    EXPECT_GE(bbox.y0, 0.0f);
}

// ============================================================================
// TC-059: MQTT ROI Format Validation
// ============================================================================

TEST_F(PublisherTest, MqttROIFormat) {
    // Test ROI (Region of Interest) format
    struct ROI {
        std::string type;       // "border" or "polygon"
        float       border_frac; // 0.0 to 0.5
        float       rect[4];     // [x0, y0, x1, y1]
    };

    // Test border type
    ROI border_roi;
    border_roi.type = "border";
    border_roi.border_frac = 0.10f;  // 10% inset
    border_roi.rect[0] = 128.0f;
    border_roi.rect[1] = 72.0f;
    border_roi.rect[2] = 1152.0f;
    border_roi.rect[3] = 648.0f;

    EXPECT_EQ(border_roi.type, "border");
    EXPECT_GE(border_roi.border_frac, 0.0f) << "Border fraction >= 0";
    EXPECT_LE(border_roi.border_frac, 0.5f) << "Border fraction <= 0.5";

    // Validate rect format
    EXPECT_LT(border_roi.rect[0], border_roi.rect[2]) << "x0 < x1";
    EXPECT_LT(border_roi.rect[1], border_roi.rect[3]) << "y0 < y1";
}

// ============================================================================
// TC-060: MQTT Gaze Object Format
// ============================================================================

TEST_F(PublisherTest, MqttGazeObjectFormat) {
    // Test gaze object format (optional per-track field)
    struct GazeData {
        int     detected;      // 0 or 1
        float   time;          // seconds >= 0
        float   face_bbox[4];  // [x0, y0, x1, y1]
    };

    GazeData gaze;
    gaze.detected = 1;  // Person is looking at camera
    gaze.time = 8.50f;  // 8.5 seconds of gaze time
    gaze.face_bbox[0] = 231.0f;
    gaze.face_bbox[1] = 167.0f;
    gaze.face_bbox[2] = 240.0f;
    gaze.face_bbox[3] = 180.0f;

    // Validate detected field (binary)
    EXPECT_GE(gaze.detected, 0);
    EXPECT_LE(gaze.detected, 1);

    // Validate time (non-negative)
    EXPECT_GE(gaze.time, 0.0f);

    // Validate face_bbox format
    EXPECT_LT(gaze.face_bbox[0], gaze.face_bbox[2]) << "Face x0 < x1";
    EXPECT_LT(gaze.face_bbox[1], gaze.face_bbox[3]) << "Face y0 < y1";
}

// ============================================================================
// TC-061: MQTT Health Object Format
// ============================================================================

TEST_F(PublisherTest, MqttHealthObjectFormat) {
    // Test health monitoring object format
    struct HealthData {
        float   detector_fps;
        float   tracker_fps;
        float   queue_latency_ms;
        int     dropped_frames;
        double  last_model_reload_ts;
    };

    HealthData health;
    health.detector_fps = 29.5f;
    health.tracker_fps = 29.0f;
    health.queue_latency_ms = 2.3f;
    health.dropped_frames = 0;
    health.last_model_reload_ts = 0.0;

    // Validate FPS values (should be positive)
    EXPECT_GT(health.detector_fps, 0.0f);
    EXPECT_GT(health.tracker_fps, 0.0f);

    // Validate latency (non-negative)
    EXPECT_GE(health.queue_latency_ms, 0.0f);

    // Validate dropped frames (non-negative)
    EXPECT_GE(health.dropped_frames, 0);

    // Validate timestamp (non-negative)
    EXPECT_GE(health.last_model_reload_ts, 0.0);
}
