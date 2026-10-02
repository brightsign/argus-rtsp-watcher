package config

import (
	"math"
	"os"
	"path/filepath"
	"testing"
)

func TestSearchPathsOrder(t *testing.T) {
	got := SearchPaths("/ext/watcher.json")
	want := []string{
		"/storage/sd/watcher.json",
		"/storage/sd/configs/watcher.json",
		"/storage/ssd/watcher.json",
		"/storage/ssd/configs/watcher.json",
		"/ext/watcher.json",
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
