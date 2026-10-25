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
	LogLevel      string // global engine log verbosity; empty leaves the template's value
	PreviewStride *int   // save one preview frame in every N processed frames; nil leaves the template's value
}

const mqttClientIDPrefix = "xt5-gaze"

// Topic returns the per-stream MQTT topic for a stream id.
func Topic(prefix, id string) string {
	return fmt.Sprintf("%s/%s/analytics", prefix, id)
}

// ClientID returns the per-stream MQTT client id. The broker allows one
// connection per client id, so engines sharing an id evict each other.
func ClientID(id string) string {
	return fmt.Sprintf("%s-%s", mqttClientIDPrefix, id)
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

	// Global logging verbosity: when the operator sets log_level, it governs
	// every stream's engine process; otherwise the template's value stands.
	if o.LogLevel != "" {
		m["log_level"] = o.LogLevel
	}

	// Global preview cadence: an explicit stride overrides the template's, the
	// same way log_level does.
	if o.PreviewStride != nil {
		m["frame_output_stride"] = *o.PreviewStride
	}

	// Per-stream face-detection toggle. Only an explicit value overrides the
	// template: detect_faces=false turns on the engine's test_yolo_only mode
	// (RetinaFace off, YOLOX person detection still running); true forces it off.
	if s.DetectFaces != nil {
		m["test_yolo_only"] = !*s.DetectFaces
	}

	if s.Zone != nil {
		mode := s.ZoneMode
		if mode == "" {
			mode = config.ZoneModeAny
		}
		color := s.ZoneColor
		if color == "" {
			color = config.DefaultZoneColor
		}
		thickness := s.ZoneThickness
		if thickness == 0 {
			thickness = config.DefaultZoneThickness
		}
		borderThickness := s.ZoneBorderThickness
		if borderThickness == 0 {
			borderThickness = config.DefaultZoneBorderThickness
		}
		windowMinutes := s.ZoneWindowMin
		if windowMinutes == 0 {
			windowMinutes = config.DefaultZoneWindowMinutes
		}
		m["zone"] = map[string]any{
			"x0": s.Zone[0], "y0": s.Zone[1], "x1": s.Zone[2], "y1": s.Zone[3],
			"mode": mode, "color": color, "thickness": thickness,
			"border_thickness": borderThickness, "window_minutes": windowMinutes,
		}
	}

	m["light"] = renderLight(s.Light)

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
	mqtt["client_id"] = ClientID(s.ID)
	mqtt["host"] = o.MQTTHost
	mqtt["port"] = o.MQTTPort

	return json.MarshalIndent(m, "", "  ")
}

// renderLight returns a fully-populated light object so the engine never has
// to guess; every omitted field takes its section 4.5 default.
func renderLight(l *config.LightConfig) map[string]any {
	if l == nil {
		l = &config.LightConfig{}
	}
	enabled := config.DefaultLightEnabled
	if l.Enabled != nil {
		enabled = *l.Enabled
	}
	onChange := config.DefaultLightOnChange
	if l.OnChange != nil {
		onChange = *l.OnChange
	}
	return map[string]any{
		"enabled":           enabled,
		"on_change":         onChange,
		"interval_seconds":  valueOrDefault(l.IntervalSeconds, config.DefaultLightIntervalSeconds),
		"dark_luma":         valueOrDefault(l.DarkLuma, config.DefaultLightDarkLuma),
		"light_luma":        valueOrDefault(l.LightLuma, config.DefaultLightLightLuma),
		"chroma_threshold":  valueOrDefault(l.ChromaThreshold, config.DefaultLightChromaThreshold),
		"ema_alpha":         valueOrDefault(l.EMAAlpha, config.DefaultLightEMAAlpha),
		"min_dwell_seconds": valueOrDefault(l.MinDwellSeconds, config.DefaultLightMinDwellSeconds),
		"warmup_seconds":    valueOrDefault(l.WarmupSeconds, config.DefaultLightWarmupSeconds),
		"roi_frac":          valueOrDefault(l.ROIFrac, config.DefaultLightROIFrac),
		"sample_stride":     valueOrDefault(l.SampleStride, config.DefaultLightSampleStride),
		"monochrome_camera": l.MonochromeCamera,
	}
}

func valueOrDefault[T int | float64](value *T, fallback T) T {
	if value == nil {
		return fallback
	}
	return *value
}
