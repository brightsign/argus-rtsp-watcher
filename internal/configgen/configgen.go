// Package configgen renders a per-stream engine config from the shared
// template, overlaying only the fields that differ per stream. Unknown
// template fields are preserved.
package configgen

import (
	"encoding/json"
	"fmt"
	"path/filepath"

	"argus-rtsp-watcher/internal/config"
)

// defaultFramesBaseDir is where per-stream annotated preview frames are
// written when Options.FramesBaseDir is unset.
const defaultFramesBaseDir = "/tmp/argus-frames"

// Options carries the settings shared across all rendered configs.
type Options struct {
	TemplateJSON  []byte
	TopicPrefix   string
	MQTTHost      string
	MQTTPort      int
	FramesBaseDir string
}

// Topic returns the per-stream MQTT topic for a stream id.
func Topic(prefix, id string) string {
	return fmt.Sprintf("%s/%s/analytics", prefix, id)
}

// FramesDir returns the per-stream directory the engine writes its
// annotated preview JPEG into, and that the preview server reads from.
func FramesDir(o Options, streamID string) string {
	base := o.FramesBaseDir
	if base == "" {
		base = defaultFramesBaseDir
	}
	return filepath.Join(base, streamID)
}

// Render produces a concrete engine config JSON for one stream.
func Render(s config.Stream, o Options) ([]byte, error) {
	var m map[string]any
	if err := json.Unmarshal(o.TemplateJSON, &m); err != nil {
		return nil, fmt.Errorf("parse template: %w", err)
	}

	m["device_id"] = s.ID
	m["enable_frame_output"] = true
	m["output_dir"] = FramesDir(o, s.ID)

	if s.Zone != nil {
		m["zone"] = map[string]any{
			"x0": s.Zone[0], "y0": s.Zone[1], "x1": s.Zone[2], "y1": s.Zone[3],
		}
	}

	input, ok := m["input"].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("template missing input object")
	}
	switch {
	case s.USB:
		m["input_source"] = "usb"
		// Only override capture options the operator set; the engine
		// auto-detects the USB device and defaults width/height/fps otherwise.
		if s.USBWidth > 0 || s.USBHeight > 0 || s.USBFPS > 0 {
			usb := map[string]any{}
			if s.USBWidth > 0 {
				usb["width"] = s.USBWidth
			}
			if s.USBHeight > 0 {
				usb["height"] = s.USBHeight
			}
			if s.USBFPS > 0 {
				usb["fps"] = s.USBFPS
			}
			input["usb"] = usb
		}
	case s.FilePath != "":
		m["input_source"] = "file"
		input["file_path"] = s.FilePath
	default:
		m["input_source"] = "rtsp"
		input["rtsp_url"] = s.RTSPURL
	}

	// Optional per-stream person-detection threshold: set both the YOLOX
	// detector's conf_threshold and the post-processing score gate.
	if s.Confidence != nil {
		c := *s.Confidence
		if proc, ok := m["processing"].(map[string]any); ok {
			if th, ok := proc["th"].(map[string]any); ok {
				th["score"] = c
			}
		}
		if sm, ok := m["secondary_models"].([]any); ok {
			for _, e := range sm {
				if mm, ok := e.(map[string]any); ok && mm["name"] == "yolox" {
					mm["conf_threshold"] = c
				}
			}
		}
	}

	runtime, ok := m["runtime"].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("template missing runtime object")
	}
	runtime["target_fps"] = s.TargetFPS

	pubs, ok := m["publishers"].([]any)
	if !ok || len(pubs) == 0 {
		return nil, fmt.Errorf("template missing publishers")
	}
	first, ok := pubs[0].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("template publisher[0] malformed")
	}
	mqtt, ok := first["mqtt"].(map[string]any)
	if !ok {
		return nil, fmt.Errorf("template publisher[0].mqtt missing")
	}
	mqtt["topic"] = Topic(o.TopicPrefix, s.ID)
	mqtt["host"] = o.MQTTHost
	mqtt["port"] = o.MQTTPort

	return json.MarshalIndent(m, "", "  ")
}
