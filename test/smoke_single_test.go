//go:build integration

// Package test holds hardware/on-device integration tests that drive the
// real aarch64 argus-engine binary end to end. They require the staged
// per-SoC build (bin/mosquitto + argus-engine, produced by `make build`)
// and, on any build host that is not itself aarch64, cannot execute the
// engine at all -- there is no way to run an ARM binary or exercise the
// RK3588 NPU on an x86_64 machine. Each test below documents that
// precondition and calls t.Skip with the specific reason when it is not
// met, rather than weakening or faking the assertion. On a real player (or
// an aarch64 build host with the staged assets present), the same test
// performs the real assertion: one rendered engine config drives the
// engine and it publishes analytics over MQTT.
package test

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"

	"argus-rtsp-watcher/internal/broker"
	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
	"argus-rtsp-watcher/internal/supervisor"
)

const (
	smokeStreamID    = "smoke"
	smokeTargetFPS   = 10
	smokeTopicPrefix = "bs/argus"
	smokeMQTTHost    = "127.0.0.1"

	// A non-default port keeps this test isolated from any mosquitto
	// broker already running on the host (for example a real watcher
	// under development on the same machine).
	smokeMQTTPort = 18830

	brokerReadyTimeout = 10 * time.Second
	publishTimeout     = 30 * time.Second
	engineStopGrace    = 5 * time.Second
)

// TestSingleStreamSmokeEndToEnd renders one file-input engine config via
// internal/configgen, starts mosquitto via internal/broker, launches the
// engine via supervisor.ExecLauncher, and asserts at least one analytics
// message arrives on bs/argus/smoke/analytics within publishTimeout.
func TestSingleStreamSmokeEndToEnd(t *testing.T) {
	repoRoot := repoRootDir(t)
	stagingDir := resolveStagingDir(repoRoot)
	engineBin := filepath.Join(stagingDir, "argus-engine")
	mosquittoBin := filepath.Join(stagingDir, "bin", "mosquitto")
	testVideo := filepath.Join(repoRoot, "test", "testdata", "smoke.mp4")

	requireFile(t, engineBin, "argus-engine binary",
		fmt.Sprintf("run `make build` (TARGET_SOC=%s) to produce %s", targetSOC(), stagingDir))
	requireFile(t, mosquittoBin, "bundled mosquitto broker",
		"run `make build` to stage per-SoC runtime assets alongside argus-engine")
	requireFile(t, testVideo, "smoke test video",
		"generate test/testdata/smoke.mp4, e.g.:\n"+
			"  ffmpeg -f lavfi -i testsrc=size=320x240:rate=15:duration=3 "+
			"-c:v libx264 -pix_fmt yuv420p test/testdata/smoke.mp4")

	// The staged argus-engine and mosquitto binaries are aarch64-only
	// (see Makefile build-engine / scripts/build.sh); this is the
	// documented environmental precondition from controller ruling P1,
	// not a disabled assertion. On a matching host the checks below are
	// no-ops and the real assertion runs.
	if runtime.GOARCH != "arm64" {
		t.Skipf("argus-engine at %s is an aarch64 binary; this host is GOARCH=%s and cannot execute it "+
			"(environmental precondition, ruling P1 -- run this test on an aarch64 player or build host)",
			engineBin, runtime.GOARCH)
	}

	baseConfig := filepath.Join(repoRoot, "configs", "config.json")
	baseW, err := config.Load(baseConfig)
	if err != nil {
		t.Fatalf("load %s: %v", baseConfig, err)
	}
	template, err := config.EngineTemplate(baseW, baseConfig)
	if err != nil {
		t.Fatalf("engine template from %s: %v", baseConfig, err)
	}

	workDir := t.TempDir()
	opts := configgen.Options{
		TemplateJSON: template,
		TopicPrefix:  smokeTopicPrefix,
		MQTTHost:     smokeMQTTHost,
		MQTTPort:     smokeMQTTPort,
	}
	rendered, err := configgen.Render(config.Stream{ID: smokeStreamID, FilePath: testVideo, TargetFPS: smokeTargetFPS}, opts)
	if err != nil {
		t.Fatalf("render engine config: %v", err)
	}
	configPath := filepath.Join(workDir, smokeStreamID+".json")
	if err := os.WriteFile(configPath, rendered, 0o644); err != nil {
		t.Fatalf("write rendered config: %v", err)
	}

	mosquittoConfPath := filepath.Join(workDir, "mosquitto.conf")
	mosquittoConf := fmt.Sprintf("listener %d %s\nallow_anonymous true\npersistence false\nlog_dest stdout\n",
		smokeMQTTPort, smokeMQTTHost)
	if err := os.WriteFile(mosquittoConfPath, []byte(mosquittoConf), 0o644); err != nil {
		t.Fatalf("write mosquitto conf: %v", err)
	}

	brokerCmd, err := broker.Start(mosquittoBin, mosquittoConfPath, nil)
	if err != nil {
		t.Fatalf("start mosquitto: %v", err)
	}
	t.Cleanup(func() {
		if err := broker.Stop(brokerCmd); err != nil {
			t.Logf("stop mosquitto: %v", err)
		}
	})

	brokerAddr := fmt.Sprintf("%s:%d", smokeMQTTHost, smokeMQTTPort)
	if err := broker.WaitForBroker(brokerAddr, brokerReadyTimeout); err != nil {
		t.Fatalf("mosquitto never became ready: %v", err)
	}

	launcher := supervisor.ExecLauncher{WorkDir: stagingDir}
	spec := supervisor.Spec{
		ID:          smokeStreamID,
		Bin:         engineBin,
		Args:        []string{"--config", configPath},
		Env:         engineEnviron(stagingDir, filepath.Join(workDir, "gst-registry.bin")),
		Fingerprint: "smoke",
	}
	proc, err := launcher.Start(spec)
	// A GOARCH match does not guarantee the binary can actually execute
	// (for example under emulation, or a corrupted/foreign copy); treat an
	// exec-format failure at start time the same as the proactive GOARCH
	// check above, per the brief's "attempt-and-skip-on-exec-error" option.
	if isExecFormatError(err) {
		t.Skipf("argus-engine at %s cannot execute on this host (%v) -- environmental precondition, ruling P1",
			engineBin, err)
	}
	if err != nil {
		t.Fatalf("start argus-engine: %v", err)
	}
	t.Cleanup(func() { stopProcess(t, proc) })

	topic := configgen.Topic(smokeTopicPrefix, smokeStreamID)
	if err := waitForPublish(brokerAddr, topic, publishTimeout); err != nil {
		t.Fatalf("no analytics message on %s within %s: %v", topic, publishTimeout, err)
	}
}

// repoRootDir resolves the repository root from this source file's own
// location rather than the process's working directory, so the test finds
// the same paths whether `go test` runs from the module root or the
// package directory.
func repoRootDir(t *testing.T) string {
	t.Helper()
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("resolve test source file location")
	}
	return filepath.Dir(filepath.Dir(thisFile))
}

// targetSOC returns the per-SoC staging directory name to use, matching
// the Makefile's TARGET_SOC variable (default rk3588, uppercased to match
// scripts/build.sh's SOC_LIST naming).
func targetSOC() string {
	soc := os.Getenv("TARGET_SOC")
	if soc == "" {
		soc = "rk3588"
	}
	return strings.ToUpper(soc)
}

// resolveStagingDir returns the self-contained per-SoC directory that
// `make build` assembles (argus-engine, bin/mosquitto, lib/, model/,
// config.json) -- the same layout cmd/watcher expects at
// runtime -- so this test drives the engine exactly as it ships.
// ARGUS_STAGING_DIR overrides the default for running against a
// differently-located staging tree.
func resolveStagingDir(repoRoot string) string {
	if v := os.Getenv("ARGUS_STAGING_DIR"); v != "" {
		return v
	}
	return filepath.Join(repoRoot, "staging", targetSOC())
}

// requireFile skips the test with a clear, actionable reason when path is
// missing or is not a regular file -- these are the documented
// environmental preconditions from the task brief, not disabled
// assertions.
func requireFile(t *testing.T, path, what, hint string) {
	t.Helper()
	info, err := os.Stat(path)
	if err != nil {
		t.Skipf("%s not found at %s -- %s (environmental precondition, ruling P1)", what, path, hint)
	}
	if info.IsDir() {
		t.Skipf("%s at %s is a directory, expected a file -- %s", what, path, hint)
	}
}

// isExecFormatError reports whether err stems from the kernel refusing to
// execve() a binary built for a foreign architecture.
func isExecFormatError(err error) bool {
	return err != nil && errors.Is(err, syscall.ENOEXEC)
}

// engineEnviron mirrors cmd/watcher/main.go's engineEnviron. It is
// duplicated here, not imported, because cmd/watcher is package main; both
// point at the same self-contained per-SoC staging directory. The test
// uses its own gstRegistryPath so it never collides with a real watcher's
// GStreamer plugin registry cache running on the same host.
func engineEnviron(stagingDir, gstRegistryPath string) []string {
	return []string{
		"LD_LIBRARY_PATH=" + stagingDir + "/bin:" + stagingDir + "/lib:/usr/lib",
		"GST_PLUGIN_PATH=" + stagingDir + "/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0",
		"GST_PLUGIN_SYSTEM_PATH=" + stagingDir + "/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0",
		"GST_PLUGIN_SCANNER=/usr/libexec/gstreamer-1.0/gst-plugin-scanner",
		"GST_REGISTRY_REUSE_PLUGIN_SCANNER=1",
		"GST_DEBUG_NO_COLOR=1",
		"GST_REGISTRY=" + gstRegistryPath,
	}
}

// stopProcess asks the engine to terminate, escalating to SIGKILL if it
// does not exit within engineStopGrace, mirroring supervisor's own
// shutdown behavior for a single ad hoc process.
func stopProcess(t *testing.T, proc supervisor.Process) {
	t.Helper()
	if err := proc.Signal(syscall.SIGTERM); err != nil {
		t.Logf("signal argus-engine: %v", err)
	}
	done := make(chan error, 1)
	go func() { done <- proc.Wait() }()
	select {
	case <-done:
	case <-time.After(engineStopGrace):
		if err := proc.Signal(syscall.SIGKILL); err != nil {
			t.Logf("kill argus-engine: %v", err)
		}
		<-done
	}
}

// MQTT fixed-header packet types (upper nibble; lower nibble carries
// per-type flags such as PUBLISH's QoS bits).
const (
	mqttTypeConnack = 0x20
	mqttTypePublish = 0x30
	mqttTypeSuback  = 0x90
)

// Exact fixed-header bytes for packets this client sends -- both have a
// fixed, protocol-mandated flags nibble, so no separate flag byte is
// needed for the outbound direction.
const (
	mqttFixedHeaderConnect   = 0x10
	mqttFixedHeaderSubscribe = 0x82
)

// waitForPublish is a minimal MQTT 3.1.1 client: it connects, subscribes
// to topic, and returns nil as soon as one PUBLISH for that topic arrives,
// or an error once timeout elapses. Implemented directly over TCP (rather
// than shelling out to mosquitto_sub) so the assertion does not depend on
// parsing another process's stdout, and behaves identically wherever a
// mosquitto_sub binary happens or does not happen to be on PATH.
func waitForPublish(addr, topic string, timeout time.Duration) error {
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return fmt.Errorf("dial broker: %w", err)
	}
	defer conn.Close()

	if err := conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return fmt.Errorf("set deadline: %w", err)
	}
	if err := mqttConnect(conn); err != nil {
		return fmt.Errorf("connect: %w", err)
	}
	if err := mqttSubscribe(conn, topic); err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}
	return mqttAwaitPublish(conn, topic)
}

func mqttEncodeString(s string) []byte {
	b := make([]byte, 2+len(s))
	binary.BigEndian.PutUint16(b, uint16(len(s)))
	copy(b[2:], s)
	return b
}

// mqttEncodeRemainingLength implements the MQTT variable-length integer
// encoding used in every fixed header.
func mqttEncodeRemainingLength(n int) []byte {
	var out []byte
	for {
		b := byte(n % 128)
		n /= 128
		if n > 0 {
			b |= 0x80
		}
		out = append(out, b)
		if n == 0 {
			return out
		}
	}
}

func mqttWritePacket(w io.Writer, fixedHeaderByte byte, variableAndPayload []byte) error {
	packet := append([]byte{fixedHeaderByte}, mqttEncodeRemainingLength(len(variableAndPayload))...)
	packet = append(packet, variableAndPayload...)
	_, err := w.Write(packet)
	return err
}

// mqttReadFixedHeader reads one packet's fixed header and returns its raw
// first byte (type in the upper nibble, flags in the lower) and remaining
// length, decoding the spec's multi-byte varint.
func mqttReadFixedHeader(r io.Reader) (firstByte byte, remainingLength int, err error) {
	var b [1]byte
	if _, err = io.ReadFull(r, b[:]); err != nil {
		return 0, 0, err
	}
	firstByte = b[0]
	multiplier := 1
	for {
		if _, err = io.ReadFull(r, b[:]); err != nil {
			return 0, 0, err
		}
		remainingLength += int(b[0]&0x7F) * multiplier
		if b[0]&0x80 == 0 {
			return firstByte, remainingLength, nil
		}
		multiplier *= 128
		const maxRemainingLengthBytes = 4
		if multiplier > 128*128*128*maxRemainingLengthBytes {
			return 0, 0, fmt.Errorf("malformed remaining length")
		}
	}
}

func mqttConnect(conn net.Conn) error {
	const clientID = "argus-smoke-test"
	const keepAliveSeconds = 60
	const protocolLevel311 = 0x04
	const connectFlagsCleanSession = 0x02

	var body []byte
	body = append(body, mqttEncodeString("MQTT")...)
	body = append(body, protocolLevel311)
	body = append(body, connectFlagsCleanSession)
	body = append(body, byte(keepAliveSeconds>>8), byte(keepAliveSeconds))
	body = append(body, mqttEncodeString(clientID)...)

	if err := mqttWritePacket(conn, mqttFixedHeaderConnect, body); err != nil {
		return err
	}

	typeByte, remaining, err := mqttReadFixedHeader(conn)
	if err != nil {
		return fmt.Errorf("read connack header: %w", err)
	}
	if typeByte&0xF0 != mqttTypeConnack {
		return fmt.Errorf("unexpected packet type 0x%02x, want CONNACK", typeByte)
	}
	payload := make([]byte, remaining)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return fmt.Errorf("read connack payload: %w", err)
	}
	if len(payload) < 2 {
		return fmt.Errorf("malformed connack: %d bytes", len(payload))
	}
	if returnCode := payload[1]; returnCode != 0 {
		return fmt.Errorf("broker refused connect, return code %d", returnCode)
	}
	return nil
}

func mqttSubscribe(conn net.Conn, topic string) error {
	const packetID = 1
	const requestedQoS = 0x00

	var body []byte
	body = append(body, byte(packetID>>8), byte(packetID))
	body = append(body, mqttEncodeString(topic)...)
	body = append(body, requestedQoS)

	if err := mqttWritePacket(conn, mqttFixedHeaderSubscribe, body); err != nil {
		return err
	}

	typeByte, remaining, err := mqttReadFixedHeader(conn)
	if err != nil {
		return fmt.Errorf("read suback header: %w", err)
	}
	if typeByte&0xF0 != mqttTypeSuback {
		return fmt.Errorf("unexpected packet type 0x%02x, want SUBACK", typeByte)
	}
	payload := make([]byte, remaining)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return fmt.Errorf("read suback payload: %w", err)
	}
	const subackFailureCode = 0x80
	if len(payload) < 3 {
		return fmt.Errorf("malformed suback: %d bytes", len(payload))
	}
	if payload[2] == subackFailureCode {
		return fmt.Errorf("broker rejected subscription to %q", topic)
	}
	return nil
}

// mqttAwaitPublish reads packets until a PUBLISH for wantTopic arrives.
// Only QoS 0 is requested/expected, so PUBLISH frames carry no packet
// identifier: topic length, topic, then payload.
func mqttAwaitPublish(conn net.Conn, wantTopic string) error {
	for {
		typeByte, remaining, err := mqttReadFixedHeader(conn)
		if err != nil {
			return fmt.Errorf("read packet header: %w", err)
		}
		payload := make([]byte, remaining)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return fmt.Errorf("read packet payload: %w", err)
		}
		if typeByte&0xF0 != mqttTypePublish {
			continue
		}
		if len(payload) < 2 {
			return fmt.Errorf("malformed publish: %d bytes", len(payload))
		}
		topicLen := int(binary.BigEndian.Uint16(payload[:2]))
		if len(payload) < 2+topicLen {
			return fmt.Errorf("malformed publish: topic length exceeds packet")
		}
		if string(payload[2:2+topicLen]) == wantTopic {
			return nil
		}
	}
}
