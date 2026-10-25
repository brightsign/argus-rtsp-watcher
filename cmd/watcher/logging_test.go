package main

import (
	"bytes"
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"argus-rtsp-watcher/internal/supervisor"
)

// lockedBuffer is a bytes.Buffer safe to share with the writers under test.
type lockedBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *lockedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func TestWithOutputsTagsEachSpecWithItsOwnID(t *testing.T) {
	mainLog := &lockedBuffer{}
	specs := []supervisor.Spec{
		{ID: "reo1", Fingerprint: "f1"},
		{ID: "reo1:preview", Fingerprint: "f2"},
		{ID: "mediamtx", Fingerprint: "f3"},
	}

	tagged := withOutputs(specs, mainLog)

	for _, spec := range tagged {
		if spec.Output == nil {
			t.Fatalf("spec %q has no Output", spec.ID)
		}
		spec.Output.Write([]byte("hello from " + spec.ID + "\n"))
	}
	got := mainLog.String()
	for _, want := range []string{"[reo1] hello from reo1\n", "[reo1:preview] hello from reo1:preview\n", "[mediamtx] hello from mediamtx\n"} {
		if !strings.Contains(got, want) {
			t.Errorf("main log %q is missing %q", got, want)
		}
	}
}

func TestWithOutputsDoesNotChangeFingerprintsOrTheOriginals(t *testing.T) {
	// Output is not part of a process's effective configuration: attaching it
	// must never cause a restart, and must not touch the caller's specs.
	specs := []supervisor.Spec{{ID: "a", Fingerprint: "fp-a"}, {ID: "b", Fingerprint: "fp-b"}}

	tagged := withOutputs(specs, &lockedBuffer{})

	for i := range specs {
		if specs[i].Output != nil {
			t.Errorf("original spec %q was modified", specs[i].ID)
		}
		if tagged[i].Fingerprint != specs[i].Fingerprint {
			t.Errorf("spec %q fingerprint changed: %q -> %q", specs[i].ID, specs[i].Fingerprint, tagged[i].Fingerprint)
		}
	}
}

func TestWithOutputsWithoutAMainLogLeavesChildrenInheritingStderr(t *testing.T) {
	specs := []supervisor.Spec{{ID: "a"}}

	got := withOutputs(specs, nil)

	if got[0].Output != nil {
		t.Error("Output set though there is no main log")
	}
}

func TestOpenMainLogWritesTheWatchersOwnLinesToTheConfiguredDirectory(t *testing.T) {
	dir := t.TempDir()
	defer log.SetOutput(os.Stderr)

	mainLog := openMainLog(json.RawMessage(`{"log_dir":"` + dir + `"}`))
	if mainLog == nil {
		t.Fatal("openMainLog returned nil for a writable directory")
	}
	log.Printf("watcher line for the main log")

	data, err := os.ReadFile(filepath.Join(dir, mainLogName))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "watcher line for the main log") {
		t.Errorf("main.log = %q, want the watcher's own line in it", data)
	}
}

func TestOpenMainLogFallsBackToStderrWhenTheDirectoryCannotBeCreated(t *testing.T) {
	blocker := filepath.Join(t.TempDir(), "afile")
	if err := os.WriteFile(blocker, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	var sink lockedBuffer
	log.SetOutput(&sink)
	defer log.SetOutput(os.Stderr)

	mainLog := openMainLog(json.RawMessage(`{"log_dir":"` + blocker + `/sub"}`))

	if mainLog != nil {
		t.Error("openMainLog returned a writer for an impossible directory")
	}
	if !strings.Contains(sink.String(), "main log disabled") {
		t.Errorf("no explanation logged: %q", sink.String())
	}
}

func TestOpenMainLogRejectsAMalformedTemplate(t *testing.T) {
	var sink lockedBuffer
	log.SetOutput(&sink)
	defer log.SetOutput(os.Stderr)

	if mainLog := openMainLog(json.RawMessage(`{not json`)); mainLog != nil {
		t.Error("openMainLog accepted a malformed template")
	}
}
