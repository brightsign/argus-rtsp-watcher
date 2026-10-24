#include "config/configuration.h"
#include <cstdio>
#include <fstream>
#include <3rdparty/nlohmann/json.hpp>

using json = nlohmann::json;

bool AppConfig::validate(char* err, size_t err_sz) const noexcept {
  char tmp[128]{};
  if (!primary_model.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "model invalid: %s", tmp);
    return false;
  }
  if (!processing.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "processing invalid: %s", tmp);
    return false;
  }
  if (!runtime.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "runtime invalid: %s", tmp);
    return false;
  }
  for (const auto& p : publishers) {
    if (!p.validate(tmp, sizeof(tmp))) {
      if (err&&err_sz) std::snprintf(err, err_sz, "publisher invalid: %s", tmp);
      return false;
    }
  }
  return true;
}

namespace config {
bool load_from_file(const std::string& path, AppConfig& out, bool /*strict*/, char* err, size_t err_sz) noexcept {
  // Validate path
  if (path.empty()) { 
    if (err && err_sz) std::snprintf(err, err_sz, "empty path"); 
    return false; 
  }
  
  try {
    // Read JSON file
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
      if (err && err_sz) std::snprintf(err, err_sz, "cannot open file: %s", path.c_str());
      return false;
    }
    
    json j;
    ifs >> j;
    ifs.close();
    
    // Parse test mode flags (top-level)
    if (j.contains("test_face_only") && j["test_face_only"].is_boolean()) {
      out.test_face_only = j["test_face_only"].get<bool>();
    }
    if (j.contains("test_yolo_only") && j["test_yolo_only"].is_boolean()) {
      out.test_yolo_only = j["test_yolo_only"].get<bool>();
    }
    
    // Parse other config sections as before (minimal for now)
    if (j.contains("primary_model")) {
      const auto& pm = j["primary_model"];
      if (pm.contains("model_path")) out.primary_model.model_path = pm["model_path"];
      if (pm.contains("name")) out.primary_model.name = pm["name"];
      if (pm.contains("input_size")) {
        if (pm["input_size"].contains("w")) out.primary_model.input_size.w = pm["input_size"]["w"];
        if (pm["input_size"].contains("h")) out.primary_model.input_size.h = pm["input_size"]["h"];
      }
      if (pm.contains("norm")) {
        if (pm["norm"].contains("channels")) out.primary_model.norm.channels = pm["norm"]["channels"];
      }
      if (pm.contains("conf_threshold")) out.primary_model.conf_threshold = pm["conf_threshold"];
      if (pm.contains("nms_threshold")) out.primary_model.nms_threshold = pm["nms_threshold"];
      if (pm.contains("npu_core")) out.primary_model.npu_core = pm["npu_core"].get<int>();
    }
    
    // Parse secondary models
    if (j.contains("secondary_models") && j["secondary_models"].is_array()) {
      for (const auto& sm : j["secondary_models"]) {
        ModelSpec secondary_spec;
        if (sm.contains("model_path")) secondary_spec.model_path = sm["model_path"];
        if (sm.contains("name")) secondary_spec.name = sm["name"];
        if (sm.contains("input_size")) {
          if (sm["input_size"].contains("w")) secondary_spec.input_size.w = sm["input_size"]["w"];
          if (sm["input_size"].contains("h")) secondary_spec.input_size.h = sm["input_size"]["h"];
        }
        if (sm.contains("norm")) {
          if (sm["norm"].contains("channels")) secondary_spec.norm.channels = sm["norm"]["channels"];
        }
        if (sm.contains("conf_threshold")) secondary_spec.conf_threshold = sm["conf_threshold"];
        if (sm.contains("nms_threshold")) secondary_spec.nms_threshold = sm["nms_threshold"];
        if (sm.contains("npu_core")) secondary_spec.npu_core = sm["npu_core"].get<int>();
        out.secondary_models.push_back(secondary_spec);
      }
    }
    
    if (j.contains("processing")) {
      const auto& proc = j["processing"];
      if (proc.contains("th")) {
        if (proc["th"].contains("score")) out.processing.th.score = proc["th"]["score"];
        if (proc["th"].contains("iou")) out.processing.th.iou = proc["th"]["iou"];
      }
    }
    
    if (j.contains("runtime")) {
      const auto& rt = j["runtime"];
      if (rt.contains("heartbeat_ms")) out.runtime.heartbeat_ms = rt["heartbeat_ms"];
      if (rt.contains("target_fps")) out.runtime.target_fps = rt["target_fps"];
    }
    
    if (j.contains("input")) {
      const auto& in = j["input"];
      if (in.contains("rtsp_url")) out.input.rtsp_url = in["rtsp_url"];
      // usb_device is intentionally not read from config; it is auto-detected at runtime
      if (in.contains("file_path")) out.input.file_path = in["file_path"];

      // USB capture options (width/height/fps)
      if (in.contains("usb") && in["usb"].is_object()) {
        const auto& usb = in["usb"];
        if (usb.contains("width"))  out.input.usb.width  = usb["width"];
        if (usb.contains("height")) out.input.usb.height = usb["height"];
        if (usb.contains("fps"))    out.input.usb.fps    = usb["fps"];
      }
    }
    
    // Parse input source priority
    if (j.contains("input_source_priority") && j["input_source_priority"].is_string()) {
      out.input_source_priority = j["input_source_priority"].get<std::string>();
    }
    
    // Parse device ID
    if (j.contains("device_id") && j["device_id"].is_string()) {
      out.device_id = j["device_id"].get<std::string>();
    }

    // Parse optional custom detection zone (normalized 0..1). Validation is the
    // watcher's job; here we only read it and mark it enabled when present.
    if (j.contains("zone") && j["zone"].is_object()) {
      const auto& z = j["zone"];
      if (z.contains("x0") && z.contains("y0") && z.contains("x1") && z.contains("y1")) {
        out.zone.x0 = z["x0"].get<float>();
        out.zone.y0 = z["y0"].get<float>();
        out.zone.x1 = z["x1"].get<float>();
        out.zone.y1 = z["y1"].get<float>();
        out.zone.enabled = true;
        // Optional membership mode; only "base" switches off the default "any".
        if (z.contains("mode") && z["mode"].is_string()) {
          out.zone.mode = z["mode"].get<std::string>();
        }
        // Optional "#RRGGBB" outline color; defaults to green when absent.
        if (z.contains("color") && z["color"].is_string()) {
          out.zone.color = z["color"].get<std::string>();
        }
        // Optional outline width in pixels; defaults to 1 (thin) when absent.
        if (z.contains("thickness") && z["thickness"].is_number_integer()) {
          out.zone.thickness = z["thickness"].get<int>();
        }
        // Optional red alert-border width (px), independent of the outline.
        if (z.contains("border_thickness") && z["border_thickness"].is_number_integer()) {
          out.zone.border_thickness = z["border_thickness"].get<int>();
        }
        // Optional rolling-window minutes for the on-frame detect counter.
        if (z.contains("window_minutes") && z["window_minutes"].is_number_integer()) {
          out.zone.window_minutes = z["window_minutes"].get<int>();
        }
      }
    }

    // Optional light/dark detection config; absent keys keep LightCfg defaults.
    if (j.contains("light") && j["light"].is_object()) {
      const auto& l = j["light"];
      const auto read_bool = [&l](const char* key, bool& target) {
        if (l.contains(key) && l[key].is_boolean()) target = l[key].get<bool>();
      };
      const auto read_int = [&l](const char* key, int& target) {
        if (l.contains(key) && l[key].is_number()) target = l[key].get<int>();
      };
      const auto read_float = [&l](const char* key, float& target) {
        if (l.contains(key) && l[key].is_number()) target = l[key].get<float>();
      };
      read_bool("enabled", out.light.enabled);
      read_int("interval_seconds", out.light.interval_seconds);
      read_bool("on_change", out.light.on_change);
      read_float("dark_luma", out.light.dark_luma);
      read_float("light_luma", out.light.light_luma);
      read_float("chroma_threshold", out.light.chroma_threshold);
      read_float("ema_alpha", out.light.ema_alpha);
      read_int("min_dwell_seconds", out.light.min_dwell_seconds);
      read_int("warmup_seconds", out.light.warmup_seconds);
      read_float("roi_frac", out.light.roi_frac);
      read_int("sample_stride", out.light.sample_stride);
      read_bool("monochrome_camera", out.light.monochrome_camera);
    }

    // Parse explicit input source selection (new field)
    if (j.contains("input_source") && j["input_source"].is_string()) {
      out.input_source = j["input_source"].get<std::string>();
    }

    // Bridge input_source=="usb" to a usb_device selection. The lean engine has
    // no registry, so a USB stream is requested purely via input_source (plus an
    // optional input.usb {width,height,fps}); we set a sentinel device that the
    // orchestrator resolves to a real /dev/video* node (auto-detected) before
    // opening. make_input() selects USB when usb_device is set and rtsp_url and
    // file_path are empty.
    if (out.input_source == "usb" && out.input.usb_device.empty() &&
        out.input.rtsp_url.empty() && out.input.file_path.empty()) {
      out.input.usb_device = "usb_camera";
    }
    
    // Parse log level
    if (j.contains("log_level") && j["log_level"].is_string()) {
      out.log_level = j["log_level"].get<std::string>();
    }
    if (j.contains("log_dir") && j["log_dir"].is_string()) {
      out.log_dir = j["log_dir"].get<std::string>();
    }
    if (j.contains("log_json") && j["log_json"].is_boolean()) {
      out.log_json = j["log_json"].get<bool>();
    }

    // Parse frame output options
    if (j.contains("enable_frame_output") && j["enable_frame_output"].is_boolean()) {
      out.enable_frame_output = j["enable_frame_output"].get<bool>();
    }
    if (j.contains("output_dir") && j["output_dir"].is_string()) {
      out.output_dir = j["output_dir"].get<std::string>();
    }
    if (j.contains("max_frames") && j["max_frames"].is_number_integer()) {
      out.max_frames = j["max_frames"].get<int>();
    }
    if (j.contains("frame_quality") && j["frame_quality"].is_number_integer()) {
      out.frame_quality = j["frame_quality"].get<int>();
    }
    if (j.contains("enable_frame_stamp") && j["enable_frame_stamp"].is_boolean()) {
      out.enable_frame_stamp = j["enable_frame_stamp"].get<bool>();
    }

    // Parse face blur options
    if (j.contains("blur_faces") && j["blur_faces"].is_boolean()) {
      out.blur_faces = j["blur_faces"].get<bool>();
    }
    if (j.contains("blur_method") && j["blur_method"].is_string()) {
      out.blur_method = j["blur_method"].get<std::string>();
    }
    if (j.contains("blur_intensity") && j["blur_intensity"].is_number_integer()) {
      out.blur_intensity = j["blur_intensity"].get<int>();
    }
    // Parse horizontal flip option (for front-facing/mirrored cameras)
    if (j.contains("flip_horizontal") && j["flip_horizontal"].is_boolean()) {
      out.flip_horizontal = j["flip_horizontal"].get<bool>();
    }
    // Parse employee detection (preferred, new dedicated section)
    if (j.contains("employee_detection") && j["employee_detection"].is_object()) {
      const auto& ed = j["employee_detection"];
      if (ed.contains("enabled") && ed["enabled"].is_boolean())
        out.employee_detection.enabled = ed["enabled"].get<bool>();
      if (ed.contains("model_path") && ed["model_path"].is_string())
        out.employee_detection.model_path = ed["model_path"].get<std::string>();
      if (ed.contains("npu_core") && ed["npu_core"].is_number_integer())
        out.employee_detection.npu_core = ed["npu_core"].get<int>();
    }

    // Parse uniform/vest classifier (MobileNetV3-Small) — legacy fields for backwards compatibility.
    // If the new employee_detection section is absent but the legacy fields are present,
    // migrate them into the new structure so old configs continue to work.
    if (!j.contains("employee_detection")) {
      if (j.contains("enable_uniform_model") && j["enable_uniform_model"].is_boolean()) {
        out.enable_uniform_model = j["enable_uniform_model"].get<bool>();
        out.employee_detection.enabled = out.enable_uniform_model;
      }
      if (j.contains("uniform_model") && j["uniform_model"].is_object()) {
        const auto& um = j["uniform_model"];
        if (um.contains("model_path") && um["model_path"].is_string()) {
          out.uniform_model.model_path = um["model_path"].get<std::string>();
          out.employee_detection.model_path = out.uniform_model.model_path;
        }
        if (um.contains("npu_core") && um["npu_core"].is_number_integer()) {
          out.uniform_model.npu_core = um["npu_core"].get<int>();
          out.employee_detection.npu_core = out.uniform_model.npu_core;
        }
      }
    } else {
      // New section present — also keep legacy fields in sync for any code that still reads them
      out.enable_uniform_model = out.employee_detection.enabled;
      out.uniform_model.model_path = out.employee_detection.model_path;
      out.uniform_model.npu_core   = out.employee_detection.npu_core;
    }
    
    // Parse frame output options
    if (j.contains("enable_frame_output") && j["enable_frame_output"].is_boolean()) {
      out.enable_frame_output = j["enable_frame_output"].get<bool>();
    }
    if (j.contains("output_dir") && j["output_dir"].is_string()) {
      out.output_dir = j["output_dir"].get<std::string>();
    }
    if (j.contains("max_frames") && j["max_frames"].is_number_integer()) {
      out.max_frames = j["max_frames"].get<int>();
    }
    if (j.contains("frame_quality") && j["frame_quality"].is_number_integer()) {
      out.frame_quality = j["frame_quality"].get<int>();
    }
    
    // Parse publishers array
    if (j.contains("publishers") && j["publishers"].is_array()) {
      out.publishers.clear();
      for (const auto& pub : j["publishers"]) {
        PublisherConfig pc{};
        
        // Parse kind
        if (pub.contains("kind") && pub["kind"].is_string()) {
          std::string kind_str = pub["kind"].get<std::string>();
          if (kind_str == "mqtt") pc.kind = PublisherKind::Mqtt;
          else if (kind_str == "udp") pc.kind = PublisherKind::UDP;
          else if (kind_str == "file") pc.kind = PublisherKind::File;
          else if (kind_str == "stdout") pc.kind = PublisherKind::Stdout;
        }
        
        // Parse MQTT config
        if (pub.contains("mqtt") && pub["mqtt"].is_object()) {
          const auto& mqtt = pub["mqtt"];
          if (mqtt.contains("host")) pc.mqtt.host = mqtt["host"].get<std::string>();
          if (mqtt.contains("port")) pc.mqtt.port = mqtt["port"].get<int>();
          if (mqtt.contains("client_id")) pc.mqtt.client_id = mqtt["client_id"].get<std::string>();
          if (mqtt.contains("topic")) pc.mqtt.topic = mqtt["topic"].get<std::string>();
          if (mqtt.contains("qos")) pc.mqtt.qos = mqtt["qos"].get<int>();
          if (mqtt.contains("retain")) pc.mqtt.retain = mqtt["retain"].get<bool>();
          if (mqtt.contains("period_ms")) pc.mqtt.period_ms = mqtt["period_ms"].get<int>();
          if (mqtt.contains("username")) pc.mqtt.username = mqtt["username"].get<std::string>();
          if (mqtt.contains("password")) pc.mqtt.password = mqtt["password"].get<std::string>();
          if (mqtt.contains("clean_session")) pc.mqtt.clean_session = mqtt["clean_session"].get<bool>();
        }
        
        // Parse UDP config
        if (pub.contains("udp") && pub["udp"].is_object()) {
          const auto& udp = pub["udp"];
          if (udp.contains("host")) pc.udp.host = udp["host"].get<std::string>();
          if (udp.contains("port")) pc.udp.port = udp["port"].get<uint16_t>();
        }
        
        // Parse File config
        if (pub.contains("file") && pub["file"].is_object()) {
          const auto& file = pub["file"];
          if (file.contains("dir")) pc.file.dir = file["dir"].get<std::string>();
          if (file.contains("rotate")) pc.file.rotate = file["rotate"].get<bool>();
          if (file.contains("max_bytes")) pc.file.max_bytes = file["max_bytes"].get<size_t>();
        }
        
        out.publishers.push_back(pc);
      }
    }
    
    return true;
  } catch (const std::exception& ex) {
    if (err && err_sz) std::snprintf(err, err_sz, "JSON parse error: %s", ex.what());
    return false;
  }
}
bool save_to_file(const std::string& /*path*/, const AppConfig& /*in*/, char* /*err*/, size_t /*err_sz*/) noexcept {
  return false;
}
}

