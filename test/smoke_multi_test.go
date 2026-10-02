//go:build integration

package test

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"argus-rtsp-watcher/internal/broker"
	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
)

// This test drives the compiled `watcher` binary itself (cmd/watcher), not
// the individual internal packages, so it exercises the full crash-isolation
// and live-reconciliation behavior described in docs/DESIGN.md §7.4 end to
// end -- the same aarch64-only environmental precondition as
// TestSingleStreamSmokeEndToEnd in smoke_single_test.go applies here (ruling
// P1), and this test shares that test's requireFile/isExecFormatError/
// repoRootDir/resolveStagingDir/targetSOC helpers plus the raw MQTT wire
// helpers (mqttConnect, mqttSubscribe, mqttReadFixedHeader, ...). On a
// matching host the assertions below are real: three file-input streams
// publish independently, killing one engine leaves the other two publishing
// and the killed one restarts, and rewriting watcher.json to drop cam3 and
// add cam4 is reconciled live without ever interrupting cam1/cam2.
const (
	multiTopicPrefix = "bs/argus"

	// cmd/watcher/main.go hardcodes these; there is no CLI/env override
	// (global-constraints.md), so the test dials the same fixed address
	// the real watcher binds rather than parameterizing it.
	multiWatcherMQTTAddr = "127.0.0.1:1883"

	// Mirrors cmd/watcher/main.go's unexported runtimeDir and
	// reconcileInterval constants. Duplicated here for the same reason
	// smoke_single_test.go duplicates engineEnviron: cmd/watcher is
	// package main and cannot be imported by this test.
	watcherRuntimeDir      = "/tmp/argus-watcher"
	watcherReconcileWindow = 3 * time.Second

	multiBrokerReadyTimeout    = 15 * time.Second
	multiInitialPublishTimeout = 45 * time.Second
	multiCrashRecoveryTimeout  = 45 * time.Second
	multiReconcileTimeout      = watcherReconcileWindow + 45*time.Second
	multiQuietGrace            = 5 * time.Second
	multiWatcherStopGrace      = 10 * time.Second
	multiPollInterval          = 200 * time.Millisecond
)

// TestMultiStreamIsolationAndReconcile starts the watcher against a
// 3-file-stream watcher.json (cam1/cam2/cam3), confirms all three publish
// independently, kills one engine process and confirms the other two are
// unaffected and the killed one restarts, then rewrites watcher.json in
// place to drop cam3 and add cam4 and confirms the reconcile completes
// without ever interrupting cam1/cam2.
func TestMultiStreamIsolationAndReconcile(t *testing.T) {
	repoRoot := repoRootDir(t)
	stagingDir := resolveStagingDir(repoRoot)
	engineBin := filepath.Join(stagingDir, "argus-engine")
	mosquittoBin := filepath.Join(stagingDir, "bin", "mosquitto")
	watcherBin := filepath.Join(stagingDir, "watcher")
	testVideo := filepath.Join(repoRoot, "test", "testdata", "smoke.mp4")

	requireFile(t, engineBin, "argus-engine binary",
		fmt.Sprintf("run `make build` (TARGET_SOC=%s) to produce %s", targetSOC(), stagingDir))
	requireFile(t, mosquittoBin, "bundled mosquitto broker",
		"run `make build` to stage per-SoC runtime assets alongside argus-engine")
	requireFile(t, watcherBin, "watcher binary",
		fmt.Sprintf("run `make build` (TARGET_SOC=%s) to produce %s", targetSOC(), stagingDir))
	requireFile(t, testVideo, "smoke test video",
		"generate test/testdata/smoke.mp4, e.g.:\n"+
			"  ffmpeg -f lavfi -i testsrc=size=320x240:rate=15:duration=3 "+
			"-c:v libx264 -pix_fmt yuv420p test/testdata/smoke.mp4")

	// Same documented environmental precondition as TestSingleStreamSmokeEndToEnd
	// (ruling P1): the staged watcher/argus-engine/mosquitto binaries are
	// aarch64-only and cannot execute on a non-matching host.
	if runtime.GOARCH != "arm64" {
		t.Skipf("watcher at %s is an aarch64 binary; this host is GOARCH=%s and cannot execute it "+
			"(environmental precondition, ruling P1 -- run this test on an aarch64 player or build host)",
			watcherBin, runtime.GOARCH)
	}

	// The test builds its own self-contained extension directory in a temp
	// dir and its own watcher.json inside it, rather than touching the
	// shipped examples/watcher.example.json or the shared staging/<SOC> tree
	// -- those stay untouched for any other test or a real packaging run.
	extDir := setupWatcherExtDir(t, stagingDir, watcherBin)
	watcherJSONPath := filepath.Join(extDir, "watcher.json")
	writeMultiWatcherJSON(t, watcherJSONPath, []multiStreamDef{
		{id: "cam1", filePath: testVideo, targetFPS: 10},
		{id: "cam2", filePath: testVideo, targetFPS: 10},
		{id: "cam3", filePath: testVideo, targetFPS: 10},
	})

	watcherCmd := exec.Command(watcherBin)
	watcherCmd.Dir = extDir
	watcherCmd.Stdout = os.Stdout
	watcherCmd.Stderr = os.Stderr
	watcherCmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := watcherCmd.Start(); err != nil {
		// A GOARCH match does not guarantee the binary can actually execute
		// (for example under emulation, or a corrupted/foreign copy); treat
		// an exec-format failure at start time the same as the proactive
		// GOARCH check above, per the brief's attempt-and-skip-on-exec-error
		// option (same handling as TestSingleStreamSmokeEndToEnd).
		if isExecFormatError(err) {
			t.Skipf("watcher at %s cannot execute on this host (%v) -- environmental precondition, ruling P1",
				watcherBin, err)
		}
		t.Fatalf("start watcher: %v", err)
	}
	t.Cleanup(func() { stopWatcherAndReap(t, watcherCmd, extDir) })

	if err := broker.WaitForBroker(multiWatcherMQTTAddr, multiBrokerReadyTimeout); err != nil {
		t.Fatalf("watcher's mosquitto never became ready: %v", err)
	}

	sub, err := net.DialTimeout("tcp", multiWatcherMQTTAddr, 5*time.Second)
	if err != nil {
		t.Fatalf("dial watcher's broker: %v", err)
	}
	t.Cleanup(func() { sub.Close() })
	if err := mqttConnect(sub); err != nil {
		t.Fatalf("connect to watcher's broker: %v", err)
	}
	if err := mqttSubscribe(sub, multiTopicPrefix+"/#"); err != nil {
		t.Fatalf("subscribe to %s/#: %v", multiTopicPrefix, err)
	}
	tracker := newTopicTracker()
	go trackMQTTPublishes(sub, tracker)

	cam1Topic := configgen.Topic(multiTopicPrefix, "cam1")
	cam2Topic := configgen.Topic(multiTopicPrefix, "cam2")
	cam3Topic := configgen.Topic(multiTopicPrefix, "cam3")
	cam4Topic := configgen.Topic(multiTopicPrefix, "cam4")

	if err := waitForTopics(tracker, []string{cam1Topic, cam2Topic, cam3Topic}, multiInitialPublishTimeout); err != nil {
		t.Fatalf("multi-stream startup: %v", err)
	}

	// --- Crash isolation: kill cam3's engine process; cam1/cam2 must keep
	// publishing and cam3 must restart on its own. ---
	cam3ConfigPath := filepath.Join(watcherRuntimeDir, "cam3.json")
	originalPID, err := findPIDByConfigPath(cam3ConfigPath)
	if err != nil {
		t.Fatalf("locate cam3 engine process: %v", err)
	}
	killTime := time.Now()
	if err := syscall.Kill(originalPID, syscall.SIGKILL); err != nil {
		t.Fatalf("kill cam3 engine (pid %d): %v", originalPID, err)
	}

	if err := waitForTopicAfter(tracker, cam3Topic, killTime, multiCrashRecoveryTimeout); err != nil {
		t.Fatalf("cam3 did not resume publishing after its engine was killed: %v", err)
	}
	if err := waitForTopicAfter(tracker, cam1Topic, killTime, multiCrashRecoveryTimeout); err != nil {
		t.Fatalf("cam1 stopped publishing while cam3 crashed/restarted (crash isolation broken): %v", err)
	}
	if err := waitForTopicAfter(tracker, cam2Topic, killTime, multiCrashRecoveryTimeout); err != nil {
		t.Fatalf("cam2 stopped publishing while cam3 crashed/restarted (crash isolation broken): %v", err)
	}
	restartedPID, err := findPIDByConfigPath(cam3ConfigPath)
	if err != nil {
		t.Fatalf("locate restarted cam3 engine process: %v", err)
	}
	if restartedPID == originalPID {
		t.Fatalf("cam3 engine pid %d unchanged after SIGKILL -- it did not actually restart", originalPID)
	}

	// --- Live reconcile: drop cam3, add cam4; cam1/cam2 must never stop. ---
	rewriteTime := time.Now()
	writeMultiWatcherJSON(t, watcherJSONPath, []multiStreamDef{
		{id: "cam1", filePath: testVideo, targetFPS: 10},
		{id: "cam2", filePath: testVideo, targetFPS: 10},
		{id: "cam4", filePath: testVideo, targetFPS: 10},
	})

	if err := waitForTopicAfter(tracker, cam4Topic, rewriteTime, multiReconcileTimeout); err != nil {
		t.Fatalf("cam4 did not start publishing after the live reconcile: %v", err)
	}
	if err := waitForTopicAfter(tracker, cam1Topic, rewriteTime, multiReconcileTimeout); err != nil {
		t.Fatalf("cam1 stopped publishing across the live reconcile: %v", err)
	}
	if err := waitForTopicAfter(tracker, cam2Topic, rewriteTime, multiReconcileTimeout); err != nil {
		t.Fatalf("cam2 stopped publishing across the live reconcile: %v", err)
	}

	// supervisor.Reconcile stops removed streams and waits for full process
	// exit before starting additions (internal/supervisor/supervisor.go), so
	// cam3 has already exited by the time cam4's first message proves the
	// reconcile landed. Confirm it then stays quiet rather than still
	// trickling messages from a lingering process.
	cam3CountAtReconcile := tracker.countOf(cam3Topic)
	time.Sleep(multiQuietGrace)
	if got := tracker.countOf(cam3Topic); got != cam3CountAtReconcile {
		t.Fatalf("cam3 published %d more message(s) after being removed from watcher.json -- it did not stop",
			got-cam3CountAtReconcile)
	}
}

// multiStreamDef is the subset of config.Stream this test needs to build a
// watcher.json for the live watcher process to discover and (re)load.
type multiStreamDef struct {
	id        string
	filePath  string
	targetFPS int
}

// writeMultiWatcherJSON writes (or rewrites, for the live-reconcile step) a
// watcher.json at path. Rewriting the same path updates its mtime, which is
// exactly what configwatch.Watch polls for inside the running watcher
// process to trigger a live reconcile (docs/DESIGN.md §7.4).
func writeMultiWatcherJSON(t *testing.T, path string, streams []multiStreamDef) {
	t.Helper()
	w := config.Watcher{Streams: make([]config.Stream, len(streams))}
	for i, s := range streams {
		w.Streams[i] = config.Stream{ID: s.id, FilePath: s.filePath, TargetFPS: s.targetFPS}
	}
	data, err := json.MarshalIndent(w, "", "  ")
	if err != nil {
		t.Fatalf("marshal watcher.json: %v", err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

// setupWatcherExtDir builds a self-contained extension directory for the
// watcher binary to run from. cmd/watcher/main.go resolves every per-SoC
// asset path (argus-engine, engine.template.json, bin/, lib/, model/)
// relative to os.Executable()'s own directory, and the kernel resolves that
// through /proc/self/exe to the underlying file's real location -- a
// symlinked watcher binary would therefore report the ORIGINAL staging
// directory as its own directory, not this temp one. The watcher binary
// itself is copied for that reason; everything it only reads relative to
// its directory is symlinked to avoid copying large model/library assets.
func setupWatcherExtDir(t *testing.T, stagingDir, watcherBin string) string {
	t.Helper()
	extDir := t.TempDir()

	data, err := os.ReadFile(watcherBin)
	if err != nil {
		t.Fatalf("read watcher binary %s: %v", watcherBin, err)
	}
	if err := os.WriteFile(filepath.Join(extDir, "watcher"), data, 0o755); err != nil {
		t.Fatalf("write watcher copy: %v", err)
	}

	for _, name := range []string{"argus-engine", "engine.template.json", "bin", "lib", "model"} {
		src := filepath.Join(stagingDir, name)
		if _, err := os.Stat(src); err != nil {
			t.Fatalf("staged asset missing: %s: %v", src, err)
		}
		if err := os.Symlink(src, filepath.Join(extDir, name)); err != nil {
			t.Fatalf("symlink %s: %v", src, err)
		}
	}
	return extDir
}

// stopWatcherAndReap asks the watcher process to shut down the way an
// operator would (SIGTERM, escalating to SIGKILL) -- main.go's own signal
// handler then SIGTERMs its engine children and stops mosquitto before
// exiting. As a best-effort safety net against a killed-mid-test engine or a
// watcher that failed to shut down its children cleanly, it then hunts down
// and kills any process still running out of this test's extDir, so a
// leaked child can never bind the next run's fixed MQTT/health ports.
func stopWatcherAndReap(t *testing.T, watcherCmd *exec.Cmd, extDir string) {
	t.Helper()
	if proc := watcherCmd.Process; proc != nil {
		if err := proc.Signal(syscall.SIGTERM); err != nil {
			t.Logf("signal watcher: %v", err)
		}
		done := make(chan error, 1)
		go func() { done <- watcherCmd.Wait() }()
		select {
		case <-done:
		case <-time.After(multiWatcherStopGrace):
			if err := proc.Signal(syscall.SIGKILL); err != nil {
				t.Logf("kill watcher: %v", err)
			}
			<-done
		}
	}
	killByCmdlinePrefix(extDir)
}

// killByCmdlinePrefix SIGKILLs every process whose argv[0] lives under dir.
// The watcher binary, every engine it launches, and mosquitto all execute
// from paths under this test's own extDir (each unique per t.TempDir()), so
// this reaches exactly this test's process tree and nothing else running on
// the host.
func killByCmdlinePrefix(dir string) {
	prefix := dir + string(os.PathSeparator)
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return
	}
	for _, e := range entries {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		cmdline, err := os.ReadFile(filepath.Join("/proc", e.Name(), "cmdline"))
		if err != nil || len(cmdline) == 0 {
			continue
		}
		args := strings.Split(strings.TrimRight(string(cmdline), "\x00"), "\x00")
		if len(args) == 0 || !strings.HasPrefix(args[0], prefix) {
			continue
		}
		_ = syscall.Kill(pid, syscall.SIGKILL)
	}
}

// findPIDByConfigPath scans /proc for a process invoked with the exact
// argument configPath (the engine's Spec.Args is ["--config",
// <renderedConfigPath>]), returning its pid.
func findPIDByConfigPath(configPath string) (int, error) {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return 0, fmt.Errorf("read /proc: %w", err)
	}
	for _, e := range entries {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		cmdline, err := os.ReadFile(filepath.Join("/proc", e.Name(), "cmdline"))
		if err != nil {
			continue
		}
		args := strings.Split(strings.TrimRight(string(cmdline), "\x00"), "\x00")
		for _, arg := range args {
			if arg == configPath {
				return pid, nil
			}
		}
	}
	return 0, fmt.Errorf("no running process has --config %s", configPath)
}

// topicTracker records, per MQTT topic, how many PUBLISH packets have
// arrived and when the most recent one arrived, so the test can assert both
// "this topic has published at least once" and "this topic published again
// after time T" (proving continuity or, for a removed stream, its absence).
type topicTracker struct {
	mu       sync.Mutex
	lastSeen map[string]time.Time
	count    map[string]int
}

func newTopicTracker() *topicTracker {
	return &topicTracker{lastSeen: map[string]time.Time{}, count: map[string]int{}}
}

func (tt *topicTracker) record(topic string) {
	tt.mu.Lock()
	defer tt.mu.Unlock()
	tt.lastSeen[topic] = time.Now()
	tt.count[topic]++
}

func (tt *topicTracker) lastSeenAt(topic string) (time.Time, bool) {
	tt.mu.Lock()
	defer tt.mu.Unlock()
	ts, ok := tt.lastSeen[topic]
	return ts, ok
}

func (tt *topicTracker) countOf(topic string) int {
	tt.mu.Lock()
	defer tt.mu.Unlock()
	return tt.count[topic]
}

// trackMQTTPublishes reads PUBLISH packets from a subscribed connection
// until it errors (the test closes conn on cleanup, which is the intended
// way this loop -- and its goroutine -- terminates).
func trackMQTTPublishes(conn net.Conn, tracker *topicTracker) {
	for {
		typeByte, remaining, err := mqttReadFixedHeader(conn)
		if err != nil {
			return
		}
		payload := make([]byte, remaining)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return
		}
		if typeByte&0xF0 != mqttTypePublish {
			continue
		}
		if len(payload) < 2 {
			continue
		}
		topicLen := int(binary.BigEndian.Uint16(payload[:2]))
		if len(payload) < 2+topicLen {
			continue
		}
		tracker.record(string(payload[2 : 2+topicLen]))
	}
}

// waitForTopics blocks until every topic has published at least once, or
// returns an error naming whichever topics never did within timeout.
func waitForTopics(tracker *topicTracker, topics []string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		allSeen := true
		for _, topic := range topics {
			if tracker.countOf(topic) == 0 {
				allSeen = false
				break
			}
		}
		if allSeen {
			return nil
		}
		time.Sleep(multiPollInterval)
	}
	var missing []string
	for _, topic := range topics {
		if tracker.countOf(topic) == 0 {
			missing = append(missing, topic)
		}
	}
	return fmt.Errorf("timed out after %s waiting for at least one message on: %s",
		timeout, strings.Join(missing, ", "))
}

// waitForTopicAfter blocks until topic has published strictly after the
// given instant, or returns an error once timeout elapses.
func waitForTopicAfter(tracker *topicTracker, topic string, after time.Time, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if ts, ok := tracker.lastSeenAt(topic); ok && ts.After(after) {
			return nil
		}
		time.Sleep(multiPollInterval)
	}
	return fmt.Errorf("no message on %s after %s within %s", topic, after.Format(time.RFC3339Nano), timeout)
}
