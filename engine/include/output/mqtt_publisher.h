#pragma once
#include "output/publisher_v2.h"
#include "orchestration/zone.h"
#include "tracking/tracker.h"  // For TrackedBox
#include <mosquitto.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

struct MqttPublisherCfg {
  std::string host{"127.0.0.1"};
  int         port{1883};
  std::string client_id{"xt5-gaze"};
  std::string topic{"bs/argus/analytics"};
  int         qos{1};
  bool        retain{false};
  int         period_ms{1000};
  std::string username{};
  std::string password{};
  bool        clean_session{true};
  std::string device_id{"xt5-01"};     // Device identifier for JSON
  std::string stream_id{"/dev/video1"}; // Stream identifier for JSON

  // Custom detection zone (normalized 0..1); when enabled, each track gets an
  // additive "in_zone" boolean computed from its feet.
  zone::ZoneCfg zone;
};

class MqttPublisher final : public IPublisher {
public:
  using Cfg = MqttPublisherCfg;

  explicit MqttPublisher(const Cfg& cfg) noexcept;
  ~MqttPublisher() override;

  bool start() noexcept override;
  void stop() noexcept override;

  bool publish_result(const PipelineResult& r) noexcept override;
  bool publish_telemetry(const TelemetrySnapshot& t) noexcept override;

private:
  void tick_publish() noexcept;
  std::string make_payload_locked() const;

  // Low-latency in-zone event: separate from the periodic analytics tick, this
  // fires the instant ZONE_EVENT_MIN_HITS frames with an in-zone detection land
  // within a ZONE_EVENT_WINDOW_MS lookback, so a flash does not wait for the
  // 1 Hz publish. Called per frame under m_; returns true (with the in-zone
  // count) when an event should be published, rate-limited to ZONE_EVENT_REPEAT_MS.
  bool detect_zone_event_locked(int64_t now_ms, int& in_zone_count) noexcept;
  void publish_zone_event(int in_zone_count, int64_t now_ms) noexcept;
  
  // Phase 2: Connection management
  bool reconnect_if_needed() noexcept;
  bool is_connected() const noexcept;

  Cfg cfg_;
  mosquitto* mq_{nullptr};
  std::atomic<bool> run_{false};
  std::thread tick_;

  mutable std::mutex m_;
  int      people_{0};
  int      gaze_{0};
  int      frames_accum_{0};
  int      frame_width_{640};   // V6.2: For normalized speed
  int      frame_height_{480};  // V6.2: For normalized speed
  uint64_t last_ts_ns_{0};
  uint64_t frame_seq_{0};           // Video<->MQTT sync key (see PipelineResult.seq)
  int64_t  capture_wall_ns_{0};     // Wall-clock at capture, ns (-> frame_ts, ms)
  std::vector<TrackedBox> tracks_;  // Person tracks with stable IDs

  // In-zone event detector state (see detect_zone_event_locked). The rolling
  // window defaults (1s lookback, 2 hits, 500ms repeat) live in zone::EventWindow.
  zone::EventWindow zone_window_;
  std::string zone_event_topic_;      // sibling of cfg_.topic (".../zone_event")

  // V7.0: Enhanced telemetry
  float    detector_fps_{0.0f};
  float    tracker_fps_{0.0f};
  float    npu_load_{0.0f};
  int      dropped_frames_{0};
  double   last_model_reload_ts_{0.0};
  
  // Phase 2: Performance optimizations
  mutable std::string payload_buffer_;  // Pre-allocated payload buffer (reused)
  int reconnect_attempts_{0};
  std::chrono::steady_clock::time_point last_reconnect_attempt_;
  static constexpr int MAX_RECONNECT_ATTEMPTS = 3;
  static constexpr int RECONNECT_BACKOFF_MS = 5000;  // 5 second backoff
};
