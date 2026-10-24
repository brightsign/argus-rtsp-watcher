#ifndef PUBLISHER_FACTORY_H
#define PUBLISHER_FACTORY_H

#include <memory>
#include <vector>
#include "output/publisher_v2.h"
#include "output/async_publisher.h"
#include "output/mqtt_publisher.h"
#include "config/publisher_config.h"  // from your Configuration module

// Build one publisher from config.
// Note: UDP and BrightSignV3 publishers are dropped from the lean engine
// (MQTT-only per DESIGN.md); PublisherKind::UDP falls through to nullptr.
inline PublisherPtr make_publisher(const PublisherConfig& cfg,
                                  const std::string& device_id = "xt5-01",
                                  const std::string& stream_id = "/dev/video1",
                                  const zone::ZoneCfg& zone = {},
                                  const LightCfg& light = {}) {
  switch (cfg.kind) {
    case PublisherKind::Mqtt: {
      MqttPublisher::Cfg mc{};
      mc.host         = cfg.mqtt.host;
      mc.port         = cfg.mqtt.port;
      mc.client_id    = cfg.mqtt.client_id;
      mc.topic        = cfg.mqtt.topic;
      mc.qos          = cfg.mqtt.qos;
      mc.retain       = cfg.mqtt.retain;
      mc.period_ms    = cfg.mqtt.period_ms;
      mc.username     = cfg.mqtt.username;
      mc.password     = cfg.mqtt.password;
      mc.clean_session= cfg.mqtt.clean_session;
      mc.device_id    = device_id;  // Set device identifier
      mc.stream_id    = stream_id;  // Set stream identifier
      mc.zone         = zone;
      mc.light        = light;
      return std::make_unique<MqttPublisher>(mc);
    }
    
    case PublisherKind::Stdout:
      // simple stdout publisher could be implemented in .cpp if needed
      return nullptr;
    
    default:
      return nullptr;
  }
}

// Build and wrap publishers with AsyncPublisher
inline std::vector<PublisherPtr> make_async_publishers(const std::vector<PublisherConfig>& cfgs,
                                                       size_t queue_capacity = 64,
                                                       const std::string& device_id = "xt5-01",
                                                       const std::string& stream_id = "/dev/video1",
                                                       const zone::ZoneCfg& zone = {},
                                                       const LightCfg& light = {}) {
  std::vector<PublisherPtr> out;
  out.reserve(cfgs.size());
  for (const auto& c : cfgs) {
    if (auto p = make_publisher(c, device_id, stream_id, zone, light)) {
      auto ap = std::make_unique<AsyncPublisher>(std::move(p), AsyncPublisher::Config{queue_capacity});
      out.emplace_back(std::move(ap));
    }
  }
  return out;
}

#endif // PUBLISHER_FACTORY_H

