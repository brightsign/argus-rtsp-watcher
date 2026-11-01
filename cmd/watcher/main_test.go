package main

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"

	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
	"argus-rtsp-watcher/internal/supervisor"
)

func loadTemplateForTest(t *testing.T) []byte {
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

	specs, err := buildSpecs(w, opts, "/opt/ext/argus-engine", "/opt/ext", "/cfg/config.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	// 2 streams -> 4 engine+preview specs, plus the always-on config-server spec.
	if len(specs) != 5 {
		t.Fatalf("got %d specs, want 5", len(specs))
	}
	if last := specs[len(specs)-1]; last.ID != "config-server" {
		t.Errorf("expected config-server spec last, got %q", last.ID)
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

func TestBuildSpecsAddsMediamtxWhenEnabled(t *testing.T) {
	opts := configgen.Options{TemplateJSON: loadTemplateForTest(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	extDir := t.TempDir()
	ymlPath := filepath.Join(extDir, "mediamtx.yml")
	if err := os.WriteFile(ymlPath, []byte("rtsp: yes\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	w := config.Watcher{
		Streams:  []config.Stream{{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10}},
		Mediamtx: config.Mediamtx{Enabled: true},
	}

	specs, err := buildSpecs(w, opts, filepath.Join(extDir, "argus-engine"), extDir, "/cfg/config.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	// engine + preview + mediamtx + config-server
	if len(specs) != 4 {
		t.Fatalf("got %d specs, want 4", len(specs))
	}
	var m supervisor.Spec
	for _, s := range specs {
		if s.ID == "mediamtx" {
			m = s
		}
	}
	if m.ID != "mediamtx" {
		t.Fatal("expected a mediamtx spec")
	}
	if m.Bin != filepath.Join(extDir, "mediamtx") {
		t.Errorf("mediamtx Bin=%q", m.Bin)
	}
	if len(m.Args) != 1 || m.Args[0] != ymlPath {
		t.Errorf("mediamtx Args=%v want [%s]", m.Args, ymlPath)
	}
	if m.Fingerprint == "" {
		t.Error("mediamtx spec needs a fingerprint")
	}
}

func TestBuildSpecsSkipsMediamtxWhenConfigMissing(t *testing.T) {
	opts := configgen.Options{TemplateJSON: loadTemplateForTest(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	extDir := t.TempDir() // no mediamtx.yml present
	w := config.Watcher{
		Streams:  []config.Stream{{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10}},
		Mediamtx: config.Mediamtx{Enabled: true},
	}
	specs, err := buildSpecs(w, opts, filepath.Join(extDir, "argus-engine"), extDir, "/cfg/config.json", nil)
	if err != nil {
		t.Fatalf("missing mediamtx.yml must not fail the build: %v", err)
	}
	for _, s := range specs {
		if s.ID == "mediamtx" {
			t.Error("mediamtx spec must be skipped when its config is missing")
		}
	}
}

func TestBuildSpecsHandsConfigServerTheWatcherPID(t *testing.T) {
	opts := configgen.Options{TemplateJSON: loadTemplateForTest(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	w := config.Watcher{Streams: []config.Stream{{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10}}}

	specs, err := buildSpecs(w, opts, "/opt/ext/argus-engine", "/opt/ext", "/cfg/config.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	server := specs[len(specs)-1]
	if server.ID != configServerID {
		t.Fatalf("last spec = %q, want %q", server.ID, configServerID)
	}
	want := []string{"--port", "20300", "--config", "/cfg/config.json",
		"--watcher-pid", strconv.Itoa(os.Getpid())}
	if !equalArgs(server.Args, want) {
		t.Errorf("config-server Args=%v want %v", server.Args, want)
	}
}

func TestConfigServerFingerprintIgnoresStreamsAndPID(t *testing.T) {
	// Reconciling streams must never restart the dashboard, so its fingerprint
	// depends only on the binary and the config path.
	opts := configgen.Options{TemplateJSON: loadTemplateForTest(t), TopicPrefix: "bs/argus", MQTTHost: "localhost", MQTTPort: 1883}
	one := config.Watcher{Streams: []config.Stream{{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10}}}
	two := config.Watcher{Streams: []config.Stream{
		{ID: "a", RTSPURL: "rtsp://h/a", TargetFPS: 10},
		{ID: "b", RTSPURL: "rtsp://h/b", TargetFPS: 5},
	}}

	first, err := buildSpecs(one, opts, "/opt/ext/argus-engine", "/opt/ext", "/cfg/config.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	second, err := buildSpecs(two, opts, "/opt/ext/argus-engine", "/opt/ext", "/cfg/config.json", nil)
	if err != nil {
		t.Fatal(err)
	}
	if a, b := first[len(first)-1].Fingerprint, second[len(second)-1].Fingerprint; a != b {
		t.Errorf("config-server fingerprint changed with the stream set: %s vs %s", a, b)
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
