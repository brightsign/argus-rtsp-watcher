package configgen

import (
	"encoding/json"
	"testing"

	"argus-rtsp-watcher/internal/config"
)

// loadTemplate returns the engine template (the "engine" block of the combined
// config.json), which is what configgen renders per stream.
func loadTemplate(t *testing.T) []byte {
	t.Helper()
	w, err := config.Load("../../configs/config.json")
	if err != nil {
		t.Fatal(err)
	}
	tpl, err := config.EngineTemplate(w, "../../configs/config.json")
	if err != nil {
		t.Fatal(err)
	}
	return tpl
}

func TestTopic(t *testing.T) {
	if got := Topic("bs/argus", "cam1"); got != "bs/argus/cam1/analytics" {
		t.Errorf("got %q", got)
	}
}

func TestRenderRTSPStream(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 7}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["input_source"] != "rtsp" {
		t.Errorf("input_source=%v", m["input_source"])
	}
	if m["device_id"] != "cam1" {
		t.Errorf("device_id=%v", m["device_id"])
	}
	if m["enable_frame_output"] != true {
		t.Errorf("frame output should be enabled for preview")
	}
	if m["output_dir"] != "/tmp/argus-frames/cam1" {
		t.Errorf("output_dir=%v", m["output_dir"])
	}
	in := m["input"].(map[string]any)
	if in["rtsp_url"] != "rtsp://h/live" {
		t.Errorf("rtsp_url=%v", in["rtsp_url"])
	}
	rt := m["runtime"].(map[string]any)
	if rt["target_fps"].(float64) != 7 {
		t.Errorf("target_fps=%v", rt["target_fps"])
	}
	pubs := m["publishers"].([]any)
	mq := pubs[0].(map[string]any)["mqtt"].(map[string]any)
	if mq["topic"] != "bs/argus/cam1/analytics" {
		t.Errorf("topic=%v", mq["topic"])
	}
}

func TestRenderFileStreamSetsFileSource(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "t", FilePath: "/v.mov", TargetFPS: 5}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	json.Unmarshal(out, &m)
	if m["input_source"] != "file" {
		t.Errorf("input_source=%v want file", m["input_source"])
	}
	in := m["input"].(map[string]any)
	if in["file_path"] != "/v.mov" {
		t.Errorf("file_path=%v", in["file_path"])
	}
}

func TestFramesDirUsesCustomBase(t *testing.T) {
	got := FramesDir(Options{FramesBaseDir: "/data/frames"}, "cam9")
	if got != "/data/frames/cam9" {
		t.Errorf("got %q", got)
	}
}

func TestRenderKeepsAllCoreNPU(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, _ := Render(config.Stream{ID: "c", RTSPURL: "rtsp://x", TargetFPS: 10}, opts)
	var m map[string]any
	json.Unmarshal(out, &m)
	pm := m["primary_model"].(map[string]any)
	if pm["npu_core"].(float64) != -1 {
		t.Errorf("primary npu_core=%v want -1", pm["npu_core"])
	}
}

func TestRenderZonePresent(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Zone: []float64{0.146, 0.104, 0.781, 0.972}}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	z, ok := m["zone"].(map[string]any)
	if !ok {
		t.Fatalf("zone missing or not an object: %v", m["zone"])
	}
	for k, want := range map[string]float64{"x0": 0.146, "y0": 0.104, "x1": 0.781, "y1": 0.972} {
		if got, _ := z[k].(float64); got != want {
			t.Errorf("zone.%s = %v, want %v", k, z[k], want)
		}
	}
	if got, _ := z["mode"].(string); got != config.ZoneModeAny {
		t.Errorf("zone.mode = %v, want %q (default)", z["mode"], config.ZoneModeAny)
	}
	if got, _ := z["color"].(string); got != config.DefaultZoneColor {
		t.Errorf("zone.color = %v, want %q (default)", z["color"], config.DefaultZoneColor)
	}
	if got, _ := z["thickness"].(float64); int(got) != config.DefaultZoneThickness {
		t.Errorf("zone.thickness = %v, want %d (default)", z["thickness"], config.DefaultZoneThickness)
	}
	if got, _ := z["border_thickness"].(float64); int(got) != config.DefaultZoneBorderThickness {
		t.Errorf("zone.border_thickness = %v, want %d (default)", z["border_thickness"], config.DefaultZoneBorderThickness)
	}
}

func TestRenderZoneThickness(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneThickness: 6}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	z, ok := m["zone"].(map[string]any)
	if !ok {
		t.Fatalf("zone missing or not an object: %v", m["zone"])
	}
	if got, _ := z["thickness"].(float64); int(got) != 6 {
		t.Errorf("zone.thickness = %v, want 6", z["thickness"])
	}
}

func TestRenderZoneColor(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneColor: "#ff8800"}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	z, ok := m["zone"].(map[string]any)
	if !ok {
		t.Fatalf("zone missing or not an object: %v", m["zone"])
	}
	if got, _ := z["color"].(string); got != "#ff8800" {
		t.Errorf("zone.color = %v, want %q", z["color"], "#ff8800")
	}
}

func TestRenderZoneModeBase(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneMode: config.ZoneModeBase}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	z, ok := m["zone"].(map[string]any)
	if !ok {
		t.Fatalf("zone missing or not an object: %v", m["zone"])
	}
	if got, _ := z["mode"].(string); got != config.ZoneModeBase {
		t.Errorf("zone.mode = %v, want %q", z["mode"], config.ZoneModeBase)
	}
}

func TestRenderZoneOmitted(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if _, present := m["zone"]; present {
		t.Fatalf("zone key should be absent when Stream.Zone is nil")
	}
}

func TestRenderUSB(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam", USB: true, USBWidth: 1280, USBHeight: 720, USBFPS: 30, TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["input_source"] != "usb" {
		t.Fatalf("input_source=%v, want usb", m["input_source"])
	}
	usb, ok := m["input"].(map[string]any)["usb"].(map[string]any)
	if !ok {
		t.Fatalf("input.usb missing or not an object")
	}
	if usb["width"].(float64) != 1280 || usb["height"].(float64) != 720 || usb["fps"].(float64) != 30 {
		t.Fatalf("usb opts = %v", usb)
	}
}

func TestRenderConfidence(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	c := 0.7
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10, Confidence: &c}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if got := m["processing"].(map[string]any)["th"].(map[string]any)["score"].(float64); got != 0.7 {
		t.Errorf("processing.th.score = %v, want 0.7", got)
	}
	yolox := -1.0
	for _, e := range m["secondary_models"].([]any) {
		mm := e.(map[string]any)
		if mm["name"] == "yolox" {
			yolox = mm["conf_threshold"].(float64)
		}
	}
	if yolox != 0.7 {
		t.Errorf("yolox conf_threshold = %v, want 0.7", yolox)
	}
}

func TestRenderDetectFacesDisabled(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	off := false
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10, DetectFaces: &off}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["test_yolo_only"] != true {
		t.Errorf("detect_faces=false should set test_yolo_only=true, got %v", m["test_yolo_only"])
	}
}

func TestRenderDetectFacesEnabled(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	on := true
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10, DetectFaces: &on}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["test_yolo_only"] != false {
		t.Errorf("detect_faces=true should set test_yolo_only=false, got %v", m["test_yolo_only"])
	}
}

func TestRenderDetectFacesOmittedInheritsTemplate(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	// The packaged template ships test_yolo_only=false; omitting detect_faces
	// must leave that value untouched (faces stay on).
	if m["test_yolo_only"] != false {
		t.Errorf("omitted detect_faces should inherit template test_yolo_only=false, got %v", m["test_yolo_only"])
	}
}

func TestRenderLogLevel(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883, LogLevel: "warn"}
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["log_level"] != "warn" {
		t.Errorf("log_level = %v, want warn", m["log_level"])
	}
}

func TestRenderPreviewStride(t *testing.T) {
	stride := 2
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883, PreviewStride: &stride}
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if m["frame_output_stride"] != 2.0 {
		t.Errorf("frame_output_stride = %v, want 2", m["frame_output_stride"])
	}
}

func TestRenderPreviewStrideOmittedInheritsTemplate(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	// The packaged template ships frame_output_stride=1; leaving the option unset
	// must not disturb it.
	if m["frame_output_stride"] != 1.0 {
		t.Errorf("omitted PreviewStride should inherit the template's 1, got %v", m["frame_output_stride"])
	}
}

func TestRenderLogLevelOmittedInheritsTemplate(t *testing.T) {
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(config.Stream{ID: "cam", RTSPURL: "rtsp://h/live", TargetFPS: 10}, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	// The packaged template ships log_level=info; an empty Options.LogLevel must
	// leave it in place.
	if m["log_level"] != "info" {
		t.Errorf("omitted LogLevel should inherit template log_level=info, got %v", m["log_level"])
	}
}

func renderLightObject(t *testing.T, s config.Stream) map[string]any {
	t.Helper()
	opts := Options{TemplateJSON: loadTemplate(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	out, err := Render(s, opts)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	light, ok := m["light"].(map[string]any)
	if !ok {
		t.Fatalf("light missing or not an object: %v", m["light"])
	}
	return light
}

func assertLight(t *testing.T, light map[string]any, want map[string]any) {
	t.Helper()
	if len(light) != len(want) {
		t.Errorf("light has %d fields, want %d: %v", len(light), len(want), light)
	}
	for k, w := range want {
		if light[k] != w {
			t.Errorf("light.%s = %v (%T), want %v", k, light[k], light[k], w)
		}
	}
}

func TestRenderLightDefaults(t *testing.T) {
	light := renderLightObject(t, config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10})
	assertLight(t, light, map[string]any{
		"enabled": true, "interval_seconds": 10.0, "on_change": true,
		"dark_luma": 40.0, "light_luma": 70.0, "chroma_threshold": 10.0,
		"ema_alpha": 0.1, "min_dwell_seconds": 5.0, "warmup_seconds": 3.0,
		"roi_frac": 0.6, "sample_stride": 8.0, "monochrome_camera": false,
	})
}

func TestRenderLightOverrides(t *testing.T) {
	disabled := false
	interval := 30
	light := renderLightObject(t, config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Light: &config.LightConfig{Enabled: &disabled, IntervalSeconds: &interval}})
	assertLight(t, light, map[string]any{
		"enabled": false, "interval_seconds": 30.0, "on_change": true,
		"dark_luma": 40.0, "light_luma": 70.0, "chroma_threshold": 10.0,
		"ema_alpha": 0.1, "min_dwell_seconds": 5.0, "warmup_seconds": 3.0,
		"roi_frac": 0.6, "sample_stride": 8.0, "monochrome_camera": false,
	})
}

func TestRenderLightExplicitZeroHonored(t *testing.T) {
	zero, zeroFloat := 0, 0.0
	light := renderLightObject(t, config.Stream{ID: "cam1", RTSPURL: "rtsp://h/live", TargetFPS: 10,
		Light: &config.LightConfig{DarkLuma: &zeroFloat, MinDwellSeconds: &zero, WarmupSeconds: &zero}})
	assertLight(t, light, map[string]any{
		"enabled": true, "interval_seconds": 10.0, "on_change": true,
		"dark_luma": 0.0, "light_luma": 70.0, "chroma_threshold": 10.0,
		"ema_alpha": 0.1, "min_dwell_seconds": 0.0, "warmup_seconds": 0.0,
		"roi_frac": 0.6, "sample_stride": 8.0, "monochrome_camera": false,
	})
}
