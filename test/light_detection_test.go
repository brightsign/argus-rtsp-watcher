//go:build integration

package test

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"argus-rtsp-watcher/internal/broker"
	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
	"argus-rtsp-watcher/internal/supervisor"
)

// These tests share the environmental preconditions documented in
// smoke_single_test.go: the engine is an aarch64 binary that needs the
// Rockchip NPU, so off-target each test skips with a specific reason rather
// than weakening or faking an assertion. On a real player the assertions run
// against the live engine's MQTT output.

const (
	lightMQTTPort = 18831

	// Short values keep every assertion reachable within one pass of a ~6s
	// looped clip. A fast EMA lets a 3s dark segment cross the dark threshold.
	lightWarmupSeconds  = 1
	lightDwellSeconds   = 1
	lightIntervalSecond = 1
	lightEMAAlpha       = 0.3

	// Generous relative to the 6s clip so several loops are available when
	// the engine start-up or NPU warm-up delays the first frame.
	lightObserveTimeout = 90 * time.Second

	mqttRetainFlag = 0x01
)

// lightMessage mirrors the retained bs/argus/<id>/light payload.
type lightMessage struct {
	DeviceID string  `json:"device_id"`
	Event    string  `json:"event"`
	State    string  `json:"state"`
	Level    int     `json:"level"`
	IRMode   *bool   `json:"ir_mode"`
	Luma     float64 `json:"luma"`
	Chroma   float64 `json:"chroma"`
}

type analyticsSceneLight struct {
	State  string `json:"state"`
	IRMode *bool  `json:"ir_mode"`
}

// analyticsMessage decodes only the light-related parts of an analytics
// payload; tracks stay raw so the per-track light label can be checked
// without coupling the test to every other track field.
type analyticsMessage struct {
	Light  *analyticsSceneLight `json:"light"`
	Tracks []struct {
		Light *string `json:"light"`
	} `json:"tracks"`
}

type mqttMessage struct {
	topic    string
	payload  []byte
	retained bool
}

func TestLightDetectionBrightToDark(t *testing.T) {
	session := startLightSession(t, "light-to-dark", "light_to_dark.mp4")

	var sawLight, sawDark bool
	err := session.observe(t, lightObserveTimeout, func(light *lightMessage, analytics *analyticsMessage) bool {
		if light != nil {
			switch light.State {
			case "LIGHT":
				if light.IRMode == nil || *light.IRMode {
					t.Fatalf("bright saturated colour segment must report ir_mode=false, got %+v", light)
				}
				sawLight = true
			case "DARK":
				sawDark = true
			}
		}
		return sawLight && sawDark
	})
	if err != nil {
		t.Fatalf("did not observe both LIGHT (ir_mode=false) and DARK (sawLight=%v sawDark=%v): %v", sawLight, sawDark, err)
	}

	session.assertRetainedLightState(t)
}

func TestLightDetectionIRNight(t *testing.T) {
	session := startLightSession(t, "light-ir", "ir_night.mp4")

	var sawIRDark bool
	err := session.observe(t, lightObserveTimeout, func(light *lightMessage, analytics *analyticsMessage) bool {
		if light != nil && light.State == "DARK" {
			if light.IRMode == nil || !*light.IRMode {
				t.Fatalf("bright grayscale clip is DARK and must report ir_mode=true, got %+v", light)
			}
			sawIRDark = true
		}
		if light != nil && light.State == "LIGHT" {
			t.Fatalf("bright grayscale (IR) clip must never read as LIGHT, got %+v", light)
		}
		return sawIRDark
	})
	if err != nil {
		t.Fatalf("did not observe DARK with ir_mode=true: %v", err)
	}

	session.assertRetainedLightState(t)
}

// lightSession is one running engine plus the broker coordinates a test
// needs to subscribe to it.
type lightSession struct {
	brokerAddr   string
	lightTopic   string
	analyticsTop string
	wildcard     string
}

// startLightSession brings up mosquitto and an engine looping fixtureName
// with fast light settings, skipping (never faking) when the on-target
// preconditions are unmet.
func startLightSession(t *testing.T, streamID, fixtureName string) lightSession {
	t.Helper()

	repoRoot := repoRootDir(t)
	stagingDir := resolveStagingDir(repoRoot)
	engineBin := filepath.Join(stagingDir, "argus-engine")
	mosquittoBin := filepath.Join(stagingDir, "bin", "mosquitto")
	testVideo := filepath.Join(repoRoot, "test", "testdata", fixtureName)

	requireFile(t, engineBin, "argus-engine binary",
		fmt.Sprintf("run `make build` (TARGET_SOC=%s) to produce %s", targetSOC(), stagingDir))
	requireFile(t, mosquittoBin, "bundled mosquitto broker",
		"run `make build` to stage per-SoC runtime assets alongside argus-engine")
	requireFile(t, testVideo, "light detection fixture",
		"regenerate with ffmpeg lavfi color sources (see docs for the light/dark feature)")

	if runtime.GOARCH != "arm64" {
		t.Skipf("argus-engine at %s is an aarch64 binary; this host is GOARCH=%s and cannot execute it "+
			"(environmental precondition, ruling P1 -- run this test on an aarch64 player or build host)",
			engineBin, runtime.GOARCH)
	}

	baseConfig := filepath.Join(repoRoot, "configs", "config.json")
	baseWatcher, err := config.Load(baseConfig)
	if err != nil {
		t.Fatalf("load %s: %v", baseConfig, err)
	}
	template, err := config.EngineTemplate(baseWatcher, baseConfig)
	if err != nil {
		t.Fatalf("engine template from %s: %v", baseConfig, err)
	}

	enabled := true
	onChange := true
	warmup := lightWarmupSeconds
	dwell := lightDwellSeconds
	interval := lightIntervalSecond
	alpha := lightEMAAlpha
	stream := config.Stream{
		ID:        streamID,
		FilePath:  testVideo,
		TargetFPS: smokeTargetFPS,
		Light: &config.LightConfig{
			Enabled:         &enabled,
			OnChange:        &onChange,
			WarmupSeconds:   &warmup,
			MinDwellSeconds: &dwell,
			IntervalSeconds: &interval,
			EMAAlpha:        &alpha,
		},
	}

	workDir := t.TempDir()
	opts := configgen.Options{
		TemplateJSON: template,
		TopicPrefix:  smokeTopicPrefix,
		MQTTHost:     smokeMQTTHost,
		MQTTPort:     lightMQTTPort,
	}
	rendered, err := configgen.Render(stream, opts)
	if err != nil {
		t.Fatalf("render engine config: %v", err)
	}
	// The template's file input already sets loop=true; assert it so a
	// template change cannot silently turn this into a one-shot run.
	assertFileLoops(t, rendered)

	configPath := filepath.Join(workDir, streamID+".json")
	if err := os.WriteFile(configPath, rendered, 0o644); err != nil {
		t.Fatalf("write rendered config: %v", err)
	}

	mosquittoConfPath := filepath.Join(workDir, "mosquitto.conf")
	mosquittoConf := fmt.Sprintf("listener %d %s\nallow_anonymous true\npersistence false\nlog_dest stdout\n",
		lightMQTTPort, smokeMQTTHost)
	if err := os.WriteFile(mosquittoConfPath, []byte(mosquittoConf), 0o644); err != nil {
		t.Fatalf("write mosquitto conf: %v", err)
	}

	brokerCmd, err := broker.Start(mosquittoBin, mosquittoConfPath)
	if err != nil {
		t.Fatalf("start mosquitto: %v", err)
	}
	t.Cleanup(func() {
		if err := broker.Stop(brokerCmd); err != nil {
			t.Logf("stop mosquitto: %v", err)
		}
	})

	brokerAddr := fmt.Sprintf("%s:%d", smokeMQTTHost, lightMQTTPort)
	if err := broker.WaitForBroker(brokerAddr, brokerReadyTimeout); err != nil {
		t.Fatalf("mosquitto never became ready: %v", err)
	}

	launcher := supervisor.ExecLauncher{WorkDir: stagingDir}
	spec := supervisor.Spec{
		ID:          streamID,
		Bin:         engineBin,
		Args:        []string{"--config", configPath},
		Env:         engineEnviron(stagingDir, filepath.Join(workDir, "gst-registry.bin")),
		Fingerprint: streamID,
	}
	proc, err := launcher.Start(spec)
	if isExecFormatError(err) {
		t.Skipf("argus-engine at %s cannot execute on this host (%v) -- environmental precondition, ruling P1",
			engineBin, err)
	}
	if err != nil {
		t.Fatalf("start argus-engine: %v", err)
	}
	t.Cleanup(func() { stopProcess(t, proc) })

	analyticsTopic := configgen.Topic(smokeTopicPrefix, streamID)
	return lightSession{
		brokerAddr:   brokerAddr,
		lightTopic:   fmt.Sprintf("%s/%s/light", smokeTopicPrefix, streamID),
		analyticsTop: analyticsTopic,
		wildcard:     fmt.Sprintf("%s/%s/#", smokeTopicPrefix, streamID),
	}
}

func assertFileLoops(t *testing.T, renderedConfig []byte) {
	t.Helper()
	var parsed struct {
		Input struct {
			File struct {
				Loop bool `json:"loop"`
			} `json:"file"`
		} `json:"input"`
	}
	if err := json.Unmarshal(renderedConfig, &parsed); err != nil {
		t.Fatalf("parse rendered config: %v", err)
	}
	if !parsed.Input.File.Loop {
		t.Fatalf("rendered engine config must set input.file.loop=true so the clip repeats")
	}
}

// observe subscribes to the stream's topics and feeds each decoded light or
// analytics message to visit until it returns true or timeout elapses.
// Analytics payloads are validated for the scene and per-track light fields
// as they arrive, so requirement (d) is checked on every message seen.
func (s lightSession) observe(t *testing.T, timeout time.Duration,
	visit func(light *lightMessage, analytics *analyticsMessage) bool) error {
	t.Helper()

	conn, err := net.DialTimeout("tcp", s.brokerAddr, 5*time.Second)
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
	if err := mqttSubscribe(conn, s.wildcard); err != nil {
		return fmt.Errorf("subscribe: %w", err)
	}

	var analyticsSeen int
	for {
		message, err := mqttReadPublish(conn)
		if err != nil {
			return fmt.Errorf("after %d analytics messages: %w", analyticsSeen, err)
		}
		switch message.topic {
		case s.lightTopic:
			var light lightMessage
			if err := json.Unmarshal(message.payload, &light); err != nil {
				t.Fatalf("light payload is not valid JSON: %v: %s", err, message.payload)
			}
			assertValidLightMessage(t, light)
			if visit(&light, nil) {
				return nil
			}
		case s.analyticsTop:
			var analytics analyticsMessage
			if err := json.Unmarshal(message.payload, &analytics); err != nil {
				t.Fatalf("analytics payload is not valid JSON: %v: %s", err, message.payload)
			}
			assertAnalyticsCarriesLight(t, analytics)
			analyticsSeen++
			if visit(nil, &analytics) {
				return nil
			}
		}
	}
}

// assertRetainedLightState connects a brand-new subscriber after the state
// has been published and requires the broker to hand it a retained light
// message, which is what late-joining consumers depend on.
func (s lightSession) assertRetainedLightState(t *testing.T) {
	t.Helper()

	conn, err := net.DialTimeout("tcp", s.brokerAddr, 5*time.Second)
	if err != nil {
		t.Fatalf("dial broker for late subscriber: %v", err)
	}
	defer conn.Close()
	const lateSubscriberTimeout = 10 * time.Second
	if err := conn.SetDeadline(time.Now().Add(lateSubscriberTimeout)); err != nil {
		t.Fatalf("set deadline: %v", err)
	}
	if err := mqttConnect(conn); err != nil {
		t.Fatalf("late subscriber connect: %v", err)
	}
	if err := mqttSubscribe(conn, s.lightTopic); err != nil {
		t.Fatalf("late subscriber subscribe: %v", err)
	}
	message, err := mqttReadPublish(conn)
	if err != nil {
		t.Fatalf("late subscriber received no retained light message: %v", err)
	}
	if !message.retained {
		t.Fatalf("first light message to a late subscriber must carry the retain flag")
	}
	var light lightMessage
	if err := json.Unmarshal(message.payload, &light); err != nil {
		t.Fatalf("retained light payload is not valid JSON: %v: %s", err, message.payload)
	}
	assertValidLightMessage(t, light)
}

// assertValidLightMessage requires a settled, well-formed light topic
// message. The light topic only publishes once a state is committed, so
// UNKNOWN is a defect here (unlike the analytics scene field during warmup).
func assertValidLightMessage(t *testing.T, light lightMessage) {
	t.Helper()
	if light.State != "LIGHT" && light.State != "DARK" {
		t.Fatalf("light topic state = %q, want LIGHT or DARK: %+v", light.State, light)
	}
	if light.IRMode == nil {
		t.Fatalf("light topic message lacks ir_mode: %+v", light)
	}
	if light.Event != "periodic" && light.Event != "change" {
		t.Fatalf("light topic event = %q, want periodic or change", light.Event)
	}
}

// assertAnalyticsCarriesLight requires the scene light object on every
// analytics message and, when tracks are present, a per-track light label
// that agrees with the scene state. The synthetic fixtures contain no people,
// so the per-track branch is exercised only when real tracks appear.
func assertAnalyticsCarriesLight(t *testing.T, analytics analyticsMessage) {
	t.Helper()
	if analytics.Light == nil {
		t.Fatalf("analytics message lacks scene light object")
	}
	switch analytics.Light.State {
	case "LIGHT", "DARK", "UNKNOWN":
	default:
		t.Fatalf("analytics scene light state = %q, want LIGHT, DARK or UNKNOWN", analytics.Light.State)
	}
	if analytics.Light.IRMode == nil {
		t.Fatalf("analytics scene light lacks ir_mode")
	}
	for index, track := range analytics.Tracks {
		if track.Light == nil {
			t.Fatalf("analytics track %d lacks light label", index)
		}
		if *track.Light != analytics.Light.State {
			t.Fatalf("analytics track %d light = %q, scene light = %q", index, *track.Light, analytics.Light.State)
		}
	}
}

// mqttReadPublish reads packets until a PUBLISH arrives and decodes it.
// Only QoS 0 is requested, so the broker delivers PUBLISH frames without a
// packet identifier even for QoS 1 retained messages (delivery QoS is the
// minimum of publish and subscription QoS).
func mqttReadPublish(conn net.Conn) (mqttMessage, error) {
	for {
		typeByte, remaining, err := mqttReadFixedHeader(conn)
		if err != nil {
			return mqttMessage{}, fmt.Errorf("read packet header: %w", err)
		}
		payload := make([]byte, remaining)
		if _, err := io.ReadFull(conn, payload); err != nil {
			return mqttMessage{}, fmt.Errorf("read packet payload: %w", err)
		}
		if typeByte&0xF0 != mqttTypePublish {
			continue
		}
		if len(payload) < 2 {
			return mqttMessage{}, fmt.Errorf("malformed publish: %d bytes", len(payload))
		}
		topicLength := int(binary.BigEndian.Uint16(payload[:2]))
		if len(payload) < 2+topicLength {
			return mqttMessage{}, fmt.Errorf("malformed publish: topic length exceeds packet")
		}
		return mqttMessage{
			topic:    string(payload[2 : 2+topicLength]),
			payload:  payload[2+topicLength:],
			retained: typeByte&mqttRetainFlag != 0,
		}, nil
	}
}
