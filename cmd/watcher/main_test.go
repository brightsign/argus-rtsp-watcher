package main

import (
	"os"
	"path/filepath"
	"testing"

	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
)

func loadTemplateForTest(t *testing.T) []byte {
	t.Helper()
	data, err := os.ReadFile("../../configs/config.json")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestPreviewPortAssignsSequentially(t *testing.T) {
	cases := []struct {
		index int
		want  int
	}{
		{0, 20200},
		{1, 20201},
		{3, 20203},
	}
	for _, c := range cases {
		if got := previewPort(c.index); got != c.want {
			t.Errorf("previewPort(%d)=%d want %d", c.index, got, c.want)
		}
	}
}

func TestBuildSpecsProducesEngineAndPreviewPerStream(t *testing.T) {
	opts := configgen.Options{TemplateJSON: loadTemplateForTest(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	w := config.Watcher{Streams: []config.Stream{
		{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10},
		{ID: "b", RTSPURL: "rtsp://h/b", TargetFPS: 10},
	}}

	specs, err := buildSpecs(w, opts, "/opt/ext/argus-engine", "/opt/ext", nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(specs) != 4 {
		t.Fatalf("got %d specs, want 4", len(specs))
	}

	wantIDs := []string{"a", "a:preview", "b", "b:preview"}
	for i, id := range wantIDs {
		if specs[i].ID != id {
			t.Errorf("specs[%d].ID=%q want %q", i, specs[i].ID, id)
		}
	}

	engineA, previewA, engineB, previewB := specs[0], specs[1], specs[2], specs[3]

	if engineA.Bin != "/opt/ext/argus-engine" {
		t.Errorf("engineA.Bin=%q", engineA.Bin)
	}
	wantEngineArgs := []string{"--config", filepath.Join(runtimeDir, "a.json")}
	if len(engineA.Args) != 2 || engineA.Args[0] != wantEngineArgs[0] || engineA.Args[1] != wantEngineArgs[1] {
		t.Errorf("engineA.Args=%v want %v", engineA.Args, wantEngineArgs)
	}

	wantPreviewBin := filepath.Join("/opt/ext", previewBinary)
	if previewA.Bin != wantPreviewBin {
		t.Errorf("previewA.Bin=%q want %q", previewA.Bin, wantPreviewBin)
	}
	wantPreviewAArgs := []string{"-file", filepath.Join(configgen.FramesDir(opts, "a"), "output.jpg"), "-port", "20200"}
	if !equalArgs(previewA.Args, wantPreviewAArgs) {
		t.Errorf("previewA.Args=%v want %v", previewA.Args, wantPreviewAArgs)
	}
	wantPreviewBArgs := []string{"-file", filepath.Join(configgen.FramesDir(opts, "b"), "output.jpg"), "-port", "20201"}
	if !equalArgs(previewB.Args, wantPreviewBArgs) {
		t.Errorf("previewB.Args=%v want %v", previewB.Args, wantPreviewBArgs)
	}

	if engineB.Fingerprint == "" || previewA.Fingerprint == "" {
		t.Errorf("expected non-empty fingerprints")
	}
	if previewA.Fingerprint == previewB.Fingerprint {
		t.Errorf("distinct streams' preview specs must have distinct fingerprints")
	}

	for _, id := range []string{"a", "b"} {
		if info, err := os.Stat(configgen.FramesDir(opts, id)); err != nil || !info.IsDir() {
			t.Errorf("frames dir for %s not created: %v", id, err)
		}
	}
}

func equalArgs(got, want []string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}
