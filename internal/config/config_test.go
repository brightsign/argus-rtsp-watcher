package config

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"testing"
)

func TestEngineTemplateOwnAndFallback(t *testing.T) {
	dir := t.TempDir()
	pkg := filepath.Join(dir, "config.json")
	if err := os.WriteFile(pkg, []byte(`{"streams":[{"id":"a","rtsp_url":"rtsp://x"}],"engine":{"input_source":"rtsp"}}`), 0o644); err != nil {
		t.Fatal(err)
	}
	// A document with its own engine block uses it verbatim.
	own := Watcher{Engine: json.RawMessage(`{"input_source":"usb"}`)}
	tpl, err := EngineTemplate(own, pkg)
	if err != nil || string(tpl) != `{"input_source":"usb"}` {
		t.Fatalf("own engine: got %q err %v", tpl, err)
	}
	// A document without an engine block falls back to the packaged default.
	tpl, err = EngineTemplate(Watcher{}, pkg)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(tpl, &m); err != nil || m["input_source"] != "rtsp" {
		t.Errorf("fallback engine=%v err %v", m, err)
	}
}

func TestConfigSearchPathsOrder(t *testing.T) {
	got := ConfigSearchPaths("/ext/config.json")
	want := []string{
		"/storage/sd/configs/config.json",
		"/storage/sd/config.json",
		"/storage/ssd/configs/config.json",
		"/storage/ssd/config.json",
		"/ext/config.json",
	}
	if len(got) != len(want) {
		t.Fatalf("len=%d want %d (%v)", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("path[%d]=%q want %q", i, got[i], want[i])
		}
	}
}

func TestDiscoverReturnsFirstExisting(t *testing.T) {
	dir := t.TempDir()
	second := filepath.Join(dir, "b.json")
	if err := os.WriteFile(second, []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := Discover([]string{filepath.Join(dir, "a.json"), second})
	if err != nil {
		t.Fatal(err)
	}
	if got != second {
		t.Errorf("got %q want %q", got, second)
	}
}

func TestDiscoverNoneFoundNamesPaths(t *testing.T) {
	_, err := Discover([]string{"/no/a.json", "/no/b.json"})
	if err == nil {
		t.Fatal("expected error when no config found")
	}
	if !containsAll(err.Error(), "/no/a.json", "/no/b.json") {
		t.Errorf("error should name searched paths, got: %v", err)
	}
}

func TestLoadAppliesDefaultFPS(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "w.json")
	if err := os.WriteFile(p, []byte(`{"streams":[{"id":"a","rtsp_url":"rtsp://x/y"}]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	w, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	if w.Streams[0].TargetFPS != DefaultTargetFPS {
		t.Errorf("fps=%d want %d", w.Streams[0].TargetFPS, DefaultTargetFPS)
	}
}

func TestLoadParsesMediamtxBlock(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "w.json")
	body := `{"streams":[{"id":"a","rtsp_url":"rtsp://x/y"}],"mediamtx":{"enabled":true,"config":"/storage/sd/configs/mediamtx.yml"}}`
	if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	w, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	if !w.Mediamtx.Enabled {
		t.Error("mediamtx.enabled should be true")
	}
	if w.Mediamtx.Config != "/storage/sd/configs/mediamtx.yml" {
		t.Errorf("mediamtx.config=%q", w.Mediamtx.Config)
	}
}

func TestLoadDefaultsMediamtxDisabled(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "w.json")
	if err := os.WriteFile(p, []byte(`{"streams":[{"id":"a","rtsp_url":"rtsp://x/y"}]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	w, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	if w.Mediamtx.Enabled {
		t.Error("mediamtx must default to disabled when omitted")
	}
}

func TestResolveMediamtxConfig(t *testing.T) {
	dir := t.TempDir()
	pkg := filepath.Join(dir, "pkg-mediamtx.yml")
	if err := os.WriteFile(pkg, []byte("rtsp: yes\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// Explicit override that exists wins.
	explicit := filepath.Join(dir, "override.yml")
	if err := os.WriteFile(explicit, []byte("rtsp: yes\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if got, err := ResolveMediamtxConfig(explicit, pkg); err != nil || got != explicit {
		t.Errorf("explicit: got %q err %v want %q", got, err, explicit)
	}
	// Missing explicit override is an error.
	if _, err := ResolveMediamtxConfig(filepath.Join(dir, "nope.yml"), pkg); err == nil {
		t.Error("expected error for missing explicit override")
	}
	// No explicit -> falls back to the package default.
	if got, err := ResolveMediamtxConfig("", pkg); err != nil || got != pkg {
		t.Errorf("default: got %q err %v want %q", got, err, pkg)
	}
}

func TestValidateRejectsDuplicateID(t *testing.T) {
	w := Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://x"}, {ID: "a", RTSPURL: "rtsp://y"}}}
	if err := w.Validate(); err == nil {
		t.Error("expected duplicate id error")
	}
}

func TestValidateRejectsEmptyID(t *testing.T) {
	w := Watcher{Streams: []Stream{{ID: "", RTSPURL: "rtsp://x"}}}
	if err := w.Validate(); err == nil {
		t.Error("expected empty id error")
	}
}

func TestValidateRejectsUnsafeID(t *testing.T) {
	w := Watcher{Streams: []Stream{{ID: "a/b", RTSPURL: "rtsp://x"}}}
	if err := w.Validate(); err == nil {
		t.Error("expected unsafe id error")
	}
}

func TestValidateRejectsWildcardID(t *testing.T) {
	tests := []string{"a+b", "a#"}
	for _, id := range tests {
		w := Watcher{Streams: []Stream{{ID: id, RTSPURL: "rtsp://x"}}}
		if err := w.Validate(); err == nil {
			t.Errorf("expected wildcard id error for %q", id)
		}
	}
}

func TestValidateRequiresSource(t *testing.T) {
	w := Watcher{Streams: []Stream{{ID: "a"}}}
	if err := w.Validate(); err == nil {
		t.Error("expected missing-source error")
	}
}

func TestLoadRejectsMalformedJSON(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(p, []byte(`{not json`), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(p); err == nil {
		t.Error("expected parse error")
	}
}

func TestValidateZone(t *testing.T) {
	withZone := func(zone []float64) Watcher {
		return Watcher{Streams: []Stream{
			{ID: "a", RTSPURL: "rtsp://h/live", TargetFPS: 10, Zone: zone},
		}}
	}
	if err := withZone([]float64{0.1, 0.1, 0.9, 0.9}).Validate(); err != nil {
		t.Fatalf("valid zone rejected: %v", err)
	}
	if err := withZone(nil).Validate(); err != nil {
		t.Fatalf("nil zone rejected: %v", err)
	}
	for _, tc := range []struct {
		name string
		zone []float64
	}{
		{"short", []float64{0.1, 0.1, 0.9}},
		{"low", []float64{-0.1, 0.1, 0.9, 0.9}},
		{"high", []float64{0.1, 0.1, 1.1, 0.9}},
		{"x0>=x1", []float64{0.9, 0.1, 0.1, 0.9}},
		{"y0>=y1", []float64{0.1, 0.9, 0.9, 0.1}},
		{"nan", []float64{math.NaN(), 0.1, 0.9, 0.9}},
	} {
		if err := withZone(tc.zone).Validate(); err == nil {
			t.Fatalf("%s: expected error, got nil", tc.name)
		}
	}
}

func TestValidateZoneMode(t *testing.T) {
	withMode := func(mode string) Watcher {
		return Watcher{Streams: []Stream{
			{ID: "a", RTSPURL: "rtsp://h/live", TargetFPS: 10,
				Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneMode: mode},
		}}
	}
	for _, ok := range []string{"", ZoneModeAny, ZoneModeBase} {
		if err := withMode(ok).Validate(); err != nil {
			t.Fatalf("zone_mode %q rejected: %v", ok, err)
		}
	}
	if err := withMode("bottom").Validate(); err == nil {
		t.Fatal("invalid zone_mode accepted")
	}
}

func TestValidateZoneColor(t *testing.T) {
	withColor := func(color string) Watcher {
		return Watcher{Streams: []Stream{
			{ID: "a", RTSPURL: "rtsp://h/live", TargetFPS: 10,
				Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneColor: color},
		}}
	}
	for _, ok := range []string{"", "#00ff00", "#FF8800", "#abcdef"} {
		if err := withColor(ok).Validate(); err != nil {
			t.Fatalf("zone_color %q rejected: %v", ok, err)
		}
	}
	for _, bad := range []string{"00ff00", "#f00", "#gggggg", "red"} {
		if err := withColor(bad).Validate(); err == nil {
			t.Fatalf("invalid zone_color %q accepted", bad)
		}
	}
}

func TestValidateZoneThickness(t *testing.T) {
	withThickness := func(n int) Watcher {
		return Watcher{Streams: []Stream{
			{ID: "a", RTSPURL: "rtsp://h/live", TargetFPS: 10,
				Zone: []float64{0.1, 0.1, 0.9, 0.9}, ZoneThickness: n},
		}}
	}
	for _, ok := range []int{0, 1, 5, MaxZoneThickness} {
		if err := withThickness(ok).Validate(); err != nil {
			t.Fatalf("zone_thickness %d rejected: %v", ok, err)
		}
	}
	for _, bad := range []int{-1, MaxZoneThickness + 1} {
		if err := withThickness(bad).Validate(); err == nil {
			t.Fatalf("invalid zone_thickness %d accepted", bad)
		}
	}
}

func TestLoadDefaultsZoneMode(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "config.json")
	if err := os.WriteFile(path, []byte(`{"streams":[{"id":"a","rtsp_url":"rtsp://h/live"}]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	w, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if w.Streams[0].ZoneMode != ZoneModeAny {
		t.Errorf("ZoneMode = %q, want %q", w.Streams[0].ZoneMode, ZoneModeAny)
	}
	if w.Streams[0].ZoneColor != DefaultZoneColor {
		t.Errorf("ZoneColor = %q, want %q", w.Streams[0].ZoneColor, DefaultZoneColor)
	}
	if w.Streams[0].ZoneThickness != DefaultZoneThickness {
		t.Errorf("ZoneThickness = %d, want %d", w.Streams[0].ZoneThickness, DefaultZoneThickness)
	}
	if w.Streams[0].ZoneBorderThickness != DefaultZoneBorderThickness {
		t.Errorf("ZoneBorderThickness = %d, want %d", w.Streams[0].ZoneBorderThickness, DefaultZoneBorderThickness)
	}
}

func containsAll(s string, subs ...string) bool {
	for _, sub := range subs {
		found := false
		for i := 0; i+len(sub) <= len(s); i++ {
			if s[i:i+len(sub)] == sub {
				found = true
				break
			}
		}
		if !found {
			return false
		}
	}
	return true
}

func TestValidateSourcesAndConfidence(t *testing.T) {
	conf := func(v float64) *float64 { return &v }
	valid := []Watcher{
		{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", TargetFPS: 10}}},
		{Streams: []Stream{{ID: "a", FilePath: "/x.mp4", TargetFPS: 10}}},
		{Streams: []Stream{{ID: "a", USB: true, USBFPS: 30, TargetFPS: 10}}},
		{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", Confidence: conf(0.4)}}},
		{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", Confidence: conf(0.0)}}},
		{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", Confidence: conf(1.0)}}},
	}
	for i, w := range valid {
		if err := w.Validate(); err != nil {
			t.Fatalf("valid[%d] rejected: %v", i, err)
		}
	}
	invalid := []struct {
		name string
		w    Watcher
	}{
		{"no source", Watcher{Streams: []Stream{{ID: "a"}}}},
		{"rtsp+usb", Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", USB: true}}}},
		{"rtsp+file", Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", FilePath: "/x"}}}},
		{"usb opts without usb", Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", USBFPS: 30}}}},
		{"conf >1", Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", Confidence: conf(1.5)}}}},
		{"conf <0", Watcher{Streams: []Stream{{ID: "a", RTSPURL: "rtsp://h", Confidence: conf(-0.1)}}}},
	}
	for _, tc := range invalid {
		if err := tc.w.Validate(); err == nil {
			t.Fatalf("%s: expected error, got nil", tc.name)
		}
	}
}
