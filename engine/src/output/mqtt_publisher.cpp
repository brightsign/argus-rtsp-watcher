#include "output/mqtt_publisher.h"
#include "metrics/log_global.h"
#include "orchestration/zone.h"
#include "analysis/light_json.h"
#include <chrono>
#include <cmath>   // For std::sqrt
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

// Only compile full implementation if mosquitto is available
#if HAVE_MOSQUITTO

using clk = std::chrono::steady_clock;

// Helper: Read NPU load percentage from RK3568 kernel debug interface
static float read_npu_load_percent() {
  std::ifstream f("/sys/kernel/debug/rknpu/load");
  if (!f.is_open()) return 0.0f;
  
  std::string line;
  if (!std::getline(f, line)) return 0.0f;
  
  // Expected format: "NPU load: XX%"
  auto colon_pos = line.find(':');
  auto percent_pos = line.find('%');
  
  if (colon_pos != std::string::npos && 
      percent_pos != std::string::npos && 
      percent_pos > colon_pos) {
    try {
      return std::stof(line.substr(colon_pos + 1, percent_pos - colon_pos - 1));
    } catch (...) {
      return 0.0f;
    }
  }
  return 0.0f;
}

MqttPublisher::MqttPublisher(const Cfg& cfg) noexcept : cfg_(cfg) {
  mosquitto_lib_init();
  // Phase 2: Pre-allocate payload buffer for typical message size
  payload_buffer_.reserve(2048);  // Enough for ~10 tracks with full schema
}

MqttPublisher::~MqttPublisher() {
  stop();
  mosquitto_lib_cleanup();
}

// Phase 2: Connection health check
bool MqttPublisher::is_connected() const noexcept {
  if (!mq_) return false;
  // Check if mosquitto connection is still alive
  return true;  // mosquitto loop handles reconnection internally
}

// Phase 2: Reconnect with exponential backoff
bool MqttPublisher::reconnect_if_needed() noexcept {
  if (is_connected()) return true;
  
  // Check backoff timer
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    now - last_reconnect_attempt_
  ).count();
  
  if (elapsed_ms < RECONNECT_BACKOFF_MS) {
    return false;  // Still in backoff period
  }
  
  if (reconnect_attempts_ >= MAX_RECONNECT_ATTEMPTS) {
    LG_WARN("mqtt: max reconnection attempts reached, giving up");
    return false;
  }
  
  last_reconnect_attempt_ = now;
  reconnect_attempts_++;
  
  LG_INFO("mqtt: attempting reconnection (attempt %d/%d)", 
          reconnect_attempts_, MAX_RECONNECT_ATTEMPTS);
  
  // Attempt reconnection
  if (mosquitto_reconnect(mq_) == MOSQ_ERR_SUCCESS) {
    LG_INFO("mqtt: reconnected successfully");
    reconnect_attempts_ = 0;  // Reset counter on success
    return true;
  }
  
  LG_WARN("mqtt: reconnection attempt %d failed", reconnect_attempts_);
  return false;
}

bool MqttPublisher::start() noexcept {
  if (mq_) {
    // Phase 2: Connection already exists, just ensure it's connected
    return reconnect_if_needed();
  }
  
  mq_ = mosquitto_new(cfg_.client_id.c_str(), cfg_.clean_session, nullptr);
  if (!mq_) { 
    LG_ERROR("mqtt: mosquitto_new failed"); 
    return false; 
  }

  if (!cfg_.username.empty())
    mosquitto_username_pw_set(mq_, cfg_.username.c_str(), cfg_.password.c_str());

  if (mosquitto_connect(mq_, cfg_.host.c_str(), cfg_.port, /*keepalive*/30) != MOSQ_ERR_SUCCESS) {
    LG_ERROR("mqtt: connect to %s:%d failed", cfg_.host.c_str(), cfg_.port);
    mosquitto_destroy(mq_); 
    mq_ = nullptr; 
    return false;
  }

  if (mosquitto_loop_start(mq_) != MOSQ_ERR_SUCCESS) {
    LG_ERROR("mqtt: loop_start failed");
    mosquitto_disconnect(mq_); 
    mosquitto_destroy(mq_); 
    mq_ = nullptr; 
    return false;
  }

  // Derive the low-latency in-zone event topic as a sibling of the analytics
  // topic: bs/argus/<id>/analytics -> bs/argus/<id>/zone_event.
  {
    const std::string suffix = "/analytics";
    if (cfg_.topic.size() >= suffix.size() &&
        cfg_.topic.compare(cfg_.topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
      zone_event_topic_ = cfg_.topic.substr(0, cfg_.topic.size() - suffix.size()) + "/zone_event";
    } else {
      zone_event_topic_ = cfg_.topic + "/zone_event";
    }
  }

  // Derive the retained light/dark state topic as a sibling of the analytics
  // topic, mirroring the zone_event derivation above.
  {
    const std::string suffix = "/analytics";
    if (cfg_.topic.size() >= suffix.size() &&
        cfg_.topic.compare(cfg_.topic.size() - suffix.size(), suffix.size(), suffix) == 0) {
      light_topic_ = cfg_.topic.substr(0, cfg_.topic.size() - suffix.size()) + "/light";
    } else {
      light_topic_ = cfg_.topic + "/light";
    }
  }

  run_.store(true, std::memory_order_release);
  tick_ = std::thread([this]{
    const int period = std::max(100, cfg_.period_ms);
    while (run_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(period));
      tick_publish();
    }
  });

  reconnect_attempts_ = 0;  // Reset reconnect counter on successful start
  LG_INFO("mqtt: connected topic=%s qos=%d retain=%d period_ms=%d",
          cfg_.topic.c_str(), cfg_.qos, int(cfg_.retain), cfg_.period_ms);
  return true;
}

void MqttPublisher::stop() noexcept {
  run_.store(false, std::memory_order_release);
  if (tick_.joinable()) tick_.join();
  if (mq_) {
    mosquitto_loop_stop(mq_, /*force=*/false);
    mosquitto_disconnect(mq_);
    mosquitto_destroy(mq_);
    mq_ = nullptr;
  }
}

bool MqttPublisher::publish_result(const PipelineResult& r) noexcept {
  bool fire = false;
  int in_zone_count = 0;
  int64_t now_ms = 0;
  bool fire_light = false;
  LightState light_to_publish{};
  {
    std::lock_guard<std::mutex> lk(m_);
    // Collect people_count (YOLOX) and gaze_count (RetinaFace)
    people_       = r.people_count;
    gaze_         = r.gaze_count;
    last_ts_ns_   = r.ts_ns;
    frame_seq_    = r.seq;             // Video<->MQTT sync key
    capture_wall_ns_ = r.capture_wall_ns;
    // Use FPS from PipelineResult instead of calculating from frames_accum_
    frames_accum_ = r.fps;  // Store actual FPS in frames_accum_ for use in payload
    frame_width_  = r.frame_width;   // V6.2: Store for normalized speed
    frame_height_ = r.frame_height;  // V6.2: Store for normalized speed

    // V7.0: Update health metrics from pipeline data (immediate, not waiting for telemetry)
    detector_fps_ = float(r.fps);  // Detector FPS ~= pipeline FPS
    tracker_fps_ = float(r.fps);   // Tracker FPS ~= pipeline FPS
    npu_load_ = read_npu_load_percent();  // Read NPU load from kernel debug interface

    // Store person tracks
    tracks_ = r.person_tracks;

    // Per-frame in-zone event detection (uses the same mode predicate as the
    // published in_zone field). Decided under the lock; published after release.
    now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                 clk::now().time_since_epoch()).count();
    fire = detect_zone_event_locked(now_ms, in_zone_count);

    // Store the latest light measurement for the periodic tick and decide the
    // on-change publish here; the mosquitto call happens after releasing m_.
    light_ = r.light;
    light_enabled_ = r.light_enabled;
    if (cfg_.light.enabled && cfg_.light.on_change) {
      const bool first = !last_light_valid_;
      if ((first && r.light.state != LightLevelState::Unknown) ||
          (!first && light_changed(last_light_, r.light))) {
        fire_light = true;
        light_to_publish = r.light;
        last_light_ = r.light;
        last_light_valid_ = true;
        last_light_pub_ms_ = now_ms;
      }
    }
  }
  if (fire) {
    publish_zone_event(in_zone_count, now_ms);
  }
  if (fire_light) {
    publish_light(light_to_publish, "change", now_ms);
  }
  return true;
}

// detect_zone_event_locked records this frame in the rolling window if any track
// is in the zone (under the configured mode), prunes the window to the last
// ZONE_EVENT_WINDOW_MS, and returns true -- rate-limited to ZONE_EVENT_REPEAT_MS
// -- once at least ZONE_EVENT_MIN_HITS frames fall inside the window.
bool MqttPublisher::detect_zone_event_locked(int64_t now_ms, int& in_zone_count) noexcept {
  in_zone_count = 0;
  if (!cfg_.zone.enabled) {
    zone_window_.reset();
    return false;
  }
  for (const auto& t : tracks_) {
    if (zone::box_in_zone(cfg_.zone.mode, t.x0, t.y0, t.x1, t.y1,
                          frame_width_, frame_height_,
                          cfg_.zone.x0, cfg_.zone.y0, cfg_.zone.x1, cfg_.zone.y1)) {
      ++in_zone_count;
    }
  }
  return zone_window_.record(in_zone_count > 0, now_ms);
}

// publish_zone_event emits a small message on the sibling zone_event topic,
// out of band from the 1 Hz analytics tick, so a flash fires immediately.
void MqttPublisher::publish_zone_event(int in_zone_count, int64_t now_ms) noexcept {
  if (!mq_ || zone_event_topic_.empty()) return;
  char buf[192];
  const int n = std::snprintf(buf, sizeof(buf),
      "{\"device_id\":\"%s\",\"event\":\"in_zone\",\"in_zone\":%d,"
      "\"mode\":\"%s\",\"ts_ms\":%lld}",
      cfg_.device_id.c_str(), in_zone_count,
      cfg_.zone.mode == zone::Mode::Base ? "base" : "any",
      static_cast<long long>(now_ms));
  if (n <= 0) return;
  const int rc = mosquitto_publish(mq_, nullptr, zone_event_topic_.c_str(),
                                   n, buf, cfg_.qos, /*retain=*/false);
  if (rc != MOSQ_ERR_SUCCESS) {
    LG_WARN("mqtt: zone_event publish failed rc=%d", rc);
  }
}

bool MqttPublisher::publish_telemetry(const TelemetrySnapshot& t) noexcept {
  std::lock_guard<std::mutex> lk(m_);
  // V7.0: Capture health metrics for MQTT output
  detector_fps_ = t.fps.avg_fps;     // Use average FPS as detector rate
  tracker_fps_ = t.fps.avg_fps;      // Same for tracker (can differentiate if needed)
  npu_load_ = t.npu_util * 100.0f;   // Convert 0-1 to 0-100%
  dropped_frames_ = 0;                // TODO: wire up from error counters if available
  // last_model_reload_ts_ remains 0.0 unless model reloading is implemented
  return true;
}

std::string MqttPublisher::make_payload_locked() const {
  // Build v7.0 JSON with full schema
  const int fps = frames_accum_;
  const double ts_s = last_ts_ns_ * 1e-9;  // Convert ns to seconds
  
  // CRITICAL FIX: Derive people count from actual tracks we're about to emit
  const int people_count = static_cast<int>(tracks_.size());
  
  // Count high-confidence tracks (score >= 0.70)
  int people_confident = 0;
  for (const auto& t : tracks_) {
    if (t.score >= 0.70f) people_confident++;
  }

  // Employee vest / uniform scene counts
  int employee_count = 0;
  int customer_count = 0;
  int unknown_uniform_count = 0;
  for (const auto& t : tracks_) {
    if (t.is_employee) {
      employee_count++;
    } else if (t.uniform_label && std::strcmp(t.uniform_label, "no_vest") == 0) {
      customer_count++;
    } else {
      unknown_uniform_count++;
    }
  }
  
  // Phase 2: Reuse pre-allocated buffer
  payload_buffer_.clear();
  payload_buffer_.reserve(1024 + tracks_.size() * 384);  // Ensure capacity for this message
  
  // V7.0: Full schema with metadata + employee scene counts
  char header[1024];
  std::snprintf(header, sizeof(header),
    "{\"schema\":\"analytics/v7.1\","
    "\"ts\":%.2f,\"device\":\"%s\",\"stream\":\"%s\","
    "\"frame_seq\":%llu,\"frame_ts\":%lld,"
    "\"frame_w\":%d,\"frame_h\":%d,"
    "\"model\":\"yolox_s\",\"fw_version\":\"7.0.0\","
    "\"npu_load\":%.1f,"
    "\"people\":%d,\"people_confident\":%d,"
    "\"gaze\":%d,\"fps\":%d,"
    "\"scene\":{\"person_count\":%d,\"employee_count\":%d,"
               "\"customer_count\":%d,\"unknown_uniform_count\":%d},"
    "\"roi\":{\"type\":\"border\",\"border_frac\":0.30,\"rect\":[%d,%d,%d,%d]},"
    "\"health\":{\"detector_fps\":%.1f,\"tracker_fps\":%.1f,\"queue_latency_ms\":0,\"dropped_frames\":%d,\"last_model_reload_ts\":%.1f},",
    ts_s,
    cfg_.device_id.c_str(),
    cfg_.stream_id.c_str(),
    // Sync key for joining recorded video to this message: frame_seq matches
    // the stamp burned into the frame; frame_ts is capture wall-clock in ms.
    static_cast<unsigned long long>(frame_seq_),
    static_cast<long long>(capture_wall_ns_ / 1000000LL),
    frame_width_, frame_height_,
    npu_load_,
    people_count, people_confident,
    gaze_, fps,
    people_count, employee_count, customer_count, unknown_uniform_count,
    // ROI rect (30% border inset)
    int(frame_width_ * 0.30f), int(frame_height_ * 0.30f),
    int(frame_width_ * 0.70f), int(frame_height_ * 0.70f),
    detector_fps_, tracker_fps_, dropped_frames_, last_model_reload_ts_);
  payload_buffer_ += header;

  // Additive scene-level light/dark summary; omitted entirely when detection is
  // off so disabled streams keep the prior message shape.
  if (light_enabled_) {
    payload_buffer_ += "\"light\":";
    payload_buffer_ += light_scene_json(light_);
    payload_buffer_ += ",";
  }
  payload_buffer_ += "\"tracks\":[";

  // Add each track with v7.1 fields
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const auto& t = tracks_[i];
    
    // V7.1b: Calculate frame diagonal once (used for normalized speed and speed floor)
    const float frame_diag = std::sqrt(float(frame_width_ * frame_width_ + frame_height_ * frame_height_));
    
    // V6.2: Calculate normalized speed (% of frame diagonal per second)
    float speed_norm = (frame_diag > 0) ? (t.speed / frame_diag) : 0.0f;
    
    // V7.0: Determine zones (simplified - just "roi" vs "edge")
    // V7.2b: Use same border_frac (30%) as tracker for consistency
    const float cx = (t.x0 + t.x1) * 0.5f;
    const float cy = (t.y0 + t.y1) * 0.5f;
    const float border_frac = 0.30f;  // Match tracker's enter_exit_border_frac
    const float border_px_x = frame_width_ * border_frac;   // 192px for 640px frame
    const float border_px_y = frame_height_ * border_frac;  // 144px for 480px frame
    const bool is_edge = (cx < border_px_x || cx > frame_width_ - border_px_x ||
                          cy < border_px_y || cy > frame_height_ - border_px_y);
    const char* zones_str = is_edge ? "[\"edge\"]" : "[\"roi\"]";

    // Custom-zone membership: the box tested against the normalized zone under
    // the configured mode (Base = feet/bottom-center inside; Any = any overlap).
    // Emitted as an additive field only when a zone is configured.
    char in_zone_field[24];
    in_zone_field[0] = '\0';
    if (cfg_.zone.enabled) {
      const bool is_in_zone = zone::box_in_zone(
          cfg_.zone.mode, t.x0, t.y0, t.x1, t.y1, frame_width_, frame_height_,
          cfg_.zone.x0, cfg_.zone.y0, cfg_.zone.x1, cfg_.zone.y1);
      std::snprintf(in_zone_field, sizeof(in_zone_field),
                    "\"in_zone\":%s,", is_in_zone ? "true" : "false");
    }

    // Per-track ambient light/dark label; additive and omitted when detection
    // is off. Carries the scene state so consumers can key off either level.
    char light_field[32];
    light_field[0] = '\0';
    if (light_enabled_) {
      std::snprintf(light_field, sizeof(light_field),
                    "\"light\":\"%s\",", light_state_str(light_.state));
    }

    // V7.1b: Strict publisher gate with resolution-adaptive threshold
    // Prevents held directions from appearing when speed dips below threshold
    // Uses diagonal-based relative floor (0.6% of frame diagonal per second)
    // 
    // V7.1d: Trust tracker's hold logic - only check confidence and label
    // Tracker handles speed floor, hold windows, and confidence decay internally
    constexpr float MIN_DIR_CONF = 0.35f;          // Match tracker's reset threshold
    const bool has_direction = (t.dir_label[0] != '?' || t.dir_label[1] != '\0');  // Not "?"
    const bool show_direction = (t.dir_conf >= MIN_DIR_CONF) && has_direction;
    
    // Publish direction from tracker (already handles hold logic)
    const char* pub_dir = show_direction ? t.dir_label : "?";
    const float pub_deg = show_direction ? t.dir_deg : 0.0f;
    const float pub_conf = show_direction ? t.dir_conf : 0.0f;
    
    // V7.1b: Cosmetic - zero tiny speeds for cleaner telemetry
    const float pub_speed = (t.speed >= 2.0f) ? t.speed : 0.0f;

    // Uniform / vest classification fields
    const char* uniform_lbl  = t.uniform_label ? t.uniform_label : "unknown";
    const float uniform_conf = t.uniform_confidence;
    const bool  is_employee  = t.is_employee;

    char buf[1024];  // Larger buffer for v7.0 + gaze + uniform data
    
    // Build gaze object if available
    if (t.has_gaze) {
      // DEBUG: Log every gaze publish for better timing visibility
      LG_INFO("[MQTT-GAZE] Track %d: gaze=%d, time=%.2f, face=[%.0f,%.0f,%.0f,%.0f]",
              t.id, t.is_gazing ? 1 : 0, t.gaze_time, 
              t.face_bbox_x0, t.face_bbox_y0, t.face_bbox_x1, t.face_bbox_y1);
      
      std::snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"state\":\"Confirmed\","
        "\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"score\":%.2f,"
        "\"zones\":%s,%s%s"
        "\"dir\":\"%s\",\"deg\":%.1f,\"dir_conf\":%.2f,"
        "\"speed\":%.1f,\"speed_norm\":%.3f,"
        "\"dwell\":%.2f,\"enter\":%s,\"exit\":%s,"
        "\"uniform\":{\"class\":\"%s\",\"confidence\":%.3f,\"is_employee\":%s},"
        "\"gaze\":{\"detected\":%d,\"time\":%.2f,\"face_bbox\":[%.1f,%.1f,%.1f,%.1f]}}",
        t.id,
        t.x0, t.y0, t.x1, t.y1, t.score,
        zones_str, in_zone_field, light_field,
        pub_dir, pub_deg, pub_conf,
        pub_speed, speed_norm,
        t.dwell_s,
        t.just_entered ? "true" : "false",
        t.just_exited ? "true" : "false",
        uniform_lbl, uniform_conf, is_employee ? "true" : "false",
        t.is_gazing ? 1 : 0,
        t.gaze_time,
        t.face_bbox_x0, t.face_bbox_y0, t.face_bbox_x1, t.face_bbox_y1);
    } else {
      // No gaze data available for this track
      static int mqtt_no_gaze_log_counter = 0;
      if (++mqtt_no_gaze_log_counter % 20 == 0) {  // Log every 20th to reduce noise
        LG_DEBUG("[MQTT-NO-GAZE] Track %d has no gaze data (has_gaze=false)", t.id);
      }
      
      std::snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"state\":\"Confirmed\","
        "\"bbox\":[%.1f,%.1f,%.1f,%.1f],\"score\":%.2f,"
        "\"zones\":%s,%s%s"
        "\"dir\":\"%s\",\"deg\":%.1f,\"dir_conf\":%.2f,"
        "\"speed\":%.1f,\"speed_norm\":%.3f,"
        "\"dwell\":%.2f,\"enter\":%s,\"exit\":%s,"
        "\"uniform\":{\"class\":\"%s\",\"confidence\":%.3f,\"is_employee\":%s}}",
        t.id,
        t.x0, t.y0, t.x1, t.y1, t.score,
        zones_str, in_zone_field, light_field,
        pub_dir, pub_deg, pub_conf,
        pub_speed, speed_norm,
        t.dwell_s,
        t.just_entered ? "true" : "false",
        t.just_exited ? "true" : "false",
        uniform_lbl, uniform_conf, is_employee ? "true" : "false");
    }
    
    if (i > 0) payload_buffer_ += ",";
    payload_buffer_ += buf;
  }
  
  payload_buffer_ += "]}";
  return payload_buffer_;
}

void MqttPublisher::tick_publish() noexcept {
  if (!mq_) return;
  std::string payload;
  bool fire_light = false;
  LightState light_to_publish{};
  int64_t now_ms = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    payload = make_payload_locked();
    // Don't reset frames_accum_ here anymore - it holds the actual FPS

    // Periodic light/dark heartbeat on its own cadence; decided under the lock,
    // published after release to avoid holding m_ across mosquitto_publish.
    if (cfg_.light.enabled) {
      now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   clk::now().time_since_epoch()).count();
      const int64_t interval_ms =
          static_cast<int64_t>(cfg_.light.interval_seconds) * 1000;
      if (now_ms - last_light_pub_ms_ >= interval_ms) {
        fire_light = true;
        light_to_publish = light_;
        last_light_ = light_;
        last_light_valid_ = true;
        last_light_pub_ms_ = now_ms;
      }
    }
  }
  const int rc = mosquitto_publish(mq_, nullptr, cfg_.topic.c_str(),
                                   int(payload.size()), payload.data(),
                                   cfg_.qos, cfg_.retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    LG_WARN("mqtt: publish failed rc=%d", rc);
  }
  if (fire_light) {
    publish_light(light_to_publish, "periodic", now_ms);
  }
}

// publish_light emits one retained light/dark message on light_topic_. qos=1 and
// retain=true are deliberate for this state topic: a late subscriber must be
// able to read the current state without waiting for the next change or tick.
void MqttPublisher::publish_light(const LightState& s, const char* event,
                                  int64_t now_ms) noexcept {
  if (!mq_ || light_topic_.empty()) return;
  const std::string payload = light_topic_json(cfg_.device_id, s, event, now_ms);
  const int rc = mosquitto_publish(mq_, nullptr, light_topic_.c_str(),
                                   int(payload.size()), payload.data(),
                                   /*qos=*/1, /*retain=*/true);
  if (rc != MOSQ_ERR_SUCCESS) {
    LG_WARN("mqtt: light publish failed rc=%d", rc);
  }
}

#endif // HAVE_MOSQUITTO
