package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// recordingSignaler stands in for the watcher so tests observe signals without
// ever signalling a real process.
type recordingSignaler struct {
	signals []syscall.Signal
	err     error
}

func (r *recordingSignaler) send(sig syscall.Signal) error {
	r.signals = append(r.signals, sig)
	return r.err
}

func newTestServer(t *testing.T, signaler *recordingSignaler) (*server, string) {
	t.Helper()
	configPath := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(configPath, []byte(`{"streams":[]}`), 0o644); err != nil {
		t.Fatal(err)
	}
	return &server{configPath: configPath, signalWatcher: signaler.send}, configPath
}

func do(t *testing.T, s *server, method, path, body string, headers map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(method, path, strings.NewReader(body))
	for name, value := range headers {
		request.Header.Set(name, value)
	}
	recorder := httptest.NewRecorder()
	s.routes().ServeHTTP(recorder, request)
	return recorder
}

func decode(t *testing.T, recorder *httptest.ResponseRecorder) map[string]bool {
	t.Helper()
	var out map[string]bool
	if err := json.Unmarshal(recorder.Body.Bytes(), &out); err != nil {
		t.Fatalf("response %q is not a JSON object of booleans: %v", recorder.Body.String(), err)
	}
	return out
}

func TestPutConfigWritesFileAndSignalsSIGHUP(t *testing.T) {
	signaler := &recordingSignaler{}
	s, configPath := newTestServer(t, signaler)
	body := `{"log_level":"warn","streams":[]}`

	recorder := do(t, s, http.MethodPut, "/config", body, nil)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	written, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if string(written) != body {
		t.Errorf("config on disk = %q, want %q", written, body)
	}
	if len(signaler.signals) != 1 || signaler.signals[0] != syscall.SIGHUP {
		t.Errorf("signals = %v, want exactly [SIGHUP]", signaler.signals)
	}
	if response := decode(t, recorder); !response["ok"] || !response["reload_signaled"] {
		t.Errorf("response = %v, want ok and reload_signaled true", response)
	}
}

func TestPutInvalidJSONIsRejectedWithoutWritingOrSignalling(t *testing.T) {
	signaler := &recordingSignaler{}
	s, configPath := newTestServer(t, signaler)

	recorder := do(t, s, http.MethodPut, "/config", `{not json`, nil)

	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", recorder.Code)
	}
	written, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if string(written) != `{"streams":[]}` {
		t.Errorf("live config was overwritten by invalid JSON: %q", written)
	}
	if len(signaler.signals) != 0 {
		t.Errorf("signals = %v, want none", signaler.signals)
	}
}

func TestPutConfigSucceedsEvenWhenWatcherCannotBeSignalled(t *testing.T) {
	// The file is saved; the watcher's file watch applies it within seconds. A
	// signalling failure must not turn a successful save into an error.
	signaler := &recordingSignaler{err: errNoWatcher}
	s, configPath := newTestServer(t, signaler)
	body := `{"streams":[{"id":"a"}]}`

	recorder := do(t, s, http.MethodPut, "/config", body, nil)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	written, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if string(written) != body {
		t.Errorf("config on disk = %q, want %q", written, body)
	}
	response := decode(t, recorder)
	if !response["ok"] || response["reload_signaled"] {
		t.Errorf("response = %v, want ok true and reload_signaled false", response)
	}
}

func TestRestartSignalsSIGUSR1(t *testing.T) {
	signaler := &recordingSignaler{}
	s, _ := newTestServer(t, signaler)

	recorder := do(t, s, http.MethodPost, "/restart", "",
		map[string]string{restartHeaderName: restartHeaderValue})

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	if len(signaler.signals) != 1 || signaler.signals[0] != syscall.SIGUSR1 {
		t.Errorf("signals = %v, want exactly [SIGUSR1]", signaler.signals)
	}
	if !decode(t, recorder)["ok"] {
		t.Errorf("response %q should report ok", recorder.Body.String())
	}
}

func TestRestartWithoutActionHeaderIsForbidden(t *testing.T) {
	// Without the custom header a cross-site form or fetch could trigger it.
	signaler := &recordingSignaler{}
	s, _ := newTestServer(t, signaler)

	for name, headers := range map[string]map[string]string{
		"no header":    nil,
		"wrong value":  {restartHeaderName: "reload"},
		"empty value":  {restartHeaderName: ""},
		"wrong header": {"X-Other": restartHeaderValue},
	} {
		recorder := do(t, s, http.MethodPost, "/restart", "", headers)
		if recorder.Code != http.StatusForbidden {
			t.Errorf("%s: status = %d, want 403", name, recorder.Code)
		}
	}
	if len(signaler.signals) != 0 {
		t.Errorf("signals = %v, want none", signaler.signals)
	}
}

func TestRestartRequiresPOST(t *testing.T) {
	signaler := &recordingSignaler{}
	s, _ := newTestServer(t, signaler)

	for _, method := range []string{http.MethodGet, http.MethodPut, http.MethodDelete} {
		recorder := do(t, s, method, "/restart", "",
			map[string]string{restartHeaderName: restartHeaderValue})
		if recorder.Code != http.StatusMethodNotAllowed {
			t.Errorf("%s: status = %d, want 405", method, recorder.Code)
		}
	}
	if len(signaler.signals) != 0 {
		t.Errorf("signals = %v, want none", signaler.signals)
	}
}

func TestRestartReportsUnavailableWhenWatcherCannotBeSignalled(t *testing.T) {
	signaler := &recordingSignaler{err: errors.New("boom")}
	s, _ := newTestServer(t, signaler)

	recorder := do(t, s, http.MethodPost, "/restart", "",
		map[string]string{restartHeaderName: restartHeaderValue})

	if recorder.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", recorder.Code)
	}
	if !strings.Contains(recorder.Body.String(), "boom") {
		t.Errorf("body %q should carry the cause", recorder.Body.String())
	}
}

func TestWatcherSignalerRefusesWithoutAWatcherPID(t *testing.T) {
	for _, pid := range []int{0, -1} {
		if err := newWatcherSignaler(pid)(syscall.SIGHUP); !errors.Is(err, errNoWatcher) {
			t.Errorf("pid %d: err = %v, want errNoWatcher", pid, err)
		}
	}
}

func TestWatcherSignalerRefusesWhenPIDIsNotTheParent(t *testing.T) {
	// A pid that is not our parent means the watcher died (its pid may be
	// reused by an unrelated process) or we run standalone: never signal it.
	notTheParent := os.Getppid() + 1
	err := newWatcherSignaler(notTheParent)(syscall.SIGHUP)
	if err == nil {
		t.Fatal("expected an error for a pid that is not the parent")
	}
	if errors.Is(err, errNoWatcher) {
		t.Errorf("err = %v; a mismatched pid is a different failure from no pid", err)
	}
}
