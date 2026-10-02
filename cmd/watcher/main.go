package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"argus-rtsp-watcher/internal/broker"
	"argus-rtsp-watcher/internal/config"
	"argus-rtsp-watcher/internal/configgen"
	"argus-rtsp-watcher/internal/configwatch"
	"argus-rtsp-watcher/internal/health"
	"argus-rtsp-watcher/internal/platform"
	"argus-rtsp-watcher/internal/supervisor"
)

const (
	topicPrefix       = "bs/argus"
	mqttHost          = "localhost"
	mqttPort          = 1883
	runtimeDir        = "/tmp/argus-watcher"
	healthAddr        = ":9200"
	brokerTimeout     = 15 * time.Second
	reconcileInterval = 3 * time.Second

	// previewBasePort is the first port handed to a stream's
	// image-stream-server; each subsequent stream gets the next port.
	previewBasePort = 20200
	previewBinary   = "image-stream-server"
	previewIDSuffix = ":preview"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("[watcher] ")

	extDir, err := os.Executable()
	if err != nil {
		log.Fatalf("locate executable: %v", err)
	}
	extDir = filepath.Dir(extDir)

	cfgPath, err := config.Discover(config.SearchPaths(filepath.Join(extDir, "watcher.json")))
	if err != nil {
		log.Fatalf("config discovery: %v", err)
	}
	log.Printf("using config %s", cfgPath)
	w, err := config.Load(cfgPath)
	if err != nil {
		log.Fatalf("config load: %v", err)
	}

	tier := platform.TierFor(platform.DetectPlayerSoC())
	log.Printf("SoC=%s model=%s safe_streams=%d configured=%d",
		tier.SoC, tier.Model, tier.SafeStreams, len(w.Streams))
	if len(w.Streams) > tier.SafeStreams {
		log.Printf("WARNING: %d streams exceeds safe capacity %d for %s",
			len(w.Streams), tier.SafeStreams, tier.Model)
	}

	tpl, err := os.ReadFile(filepath.Join(extDir, "engine.template.json"))
	if err != nil {
		log.Fatalf("read template: %v", err)
	}
	opts := configgen.Options{TemplateJSON: tpl, TopicPrefix: topicPrefix, MQTTHost: mqttHost, MQTTPort: mqttPort}

	engineBin := filepath.Join(extDir, "argus-engine")
	engineEnv := engineEnviron(extDir)

	initialSpecs, err := buildSpecs(w, opts, engineBin, extDir, engineEnv)
	if err != nil {
		log.Fatalf("build specs: %v", err)
	}

	brokerCmd, err := startBroker(extDir)
	if err != nil {
		log.Fatalf("broker: %v", err)
	}

	sup := supervisor.New(supervisor.ExecLauncher{WorkDir: extDir},
		supervisor.Backoff{Min: 250 * time.Millisecond, Max: 8 * time.Second})

	mux := http.NewServeMux()
	mux.Handle("/healthz", health.Handler(tier, len(w.Streams), sup.Status))
	go func() {
		if err := http.ListenAndServe(healthAddr, mux); err != nil {
			log.Printf("healthz server exited: %v", err)
		}
	}()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Live reconciliation: re-read watcher.json on file change or SIGHUP and
	// drive the supervisor toward the new desired stream set.
	reload := func() {
		nw, err := config.Load(cfgPath)
		if err != nil {
			log.Printf("reload skipped, invalid config: %v", err)
			return
		}
		specs, err := buildSpecs(nw, opts, engineBin, extDir, engineEnv)
		if err != nil {
			log.Printf("reload skipped, build specs: %v", err)
			return
		}
		log.Printf("reconciling to %d streams", len(specs))
		sup.Reconcile(specs)
	}
	go configwatch.Watch(ctx, cfgPath, reconcileInterval, reload)
	hup := make(chan os.Signal, 1)
	signal.Notify(hup, syscall.SIGHUP)
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case <-hup:
				reload()
			}
		}
	}()

	sup.Run(ctx, initialSpecs)
	if err := broker.Stop(brokerCmd); err != nil {
		log.Printf("broker stop: %v", err)
	}
	log.Printf("shutdown complete")
}

// previewPort returns the port assigned to the preview server of the
// stream at the given 0-based position in the configured stream list.
func previewPort(index int) int {
	return previewBasePort + index
}

// buildSpecs renders one engine config per stream, writes it to the runtime
// directory, and returns two supervisor Specs per stream: the engine itself
// and its MJPEG preview server. The engine's Fingerprint is a hash of its
// rendered config, and the preview server's is a hash of its arguments, so
// the supervisor restarts only the process whose effective config changed.
func buildSpecs(w config.Watcher, opts configgen.Options, engineBin, extDir string, engineEnv []string) ([]supervisor.Spec, error) {
	if err := os.MkdirAll(runtimeDir, 0o755); err != nil {
		return nil, err
	}
	previewBin := filepath.Join(extDir, previewBinary)
	specs := make([]supervisor.Spec, 0, len(w.Streams)*2)
	for index, s := range w.Streams {
		rendered, err := configgen.Render(s, opts)
		if err != nil {
			return nil, err
		}
		outPath := filepath.Join(runtimeDir, s.ID+".json")
		if err := os.WriteFile(outPath, rendered, 0o644); err != nil {
			return nil, err
		}
		sum := sha256.Sum256(rendered)
		env := append([]string{}, engineEnv...)
		env = append(env, "GST_REGISTRY=/tmp/gst-"+s.ID+".bin")
		specs = append(specs, supervisor.Spec{
			ID:          s.ID,
			Bin:         engineBin,
			Args:        []string{"--config", outPath},
			Env:         env,
			Fingerprint: hex.EncodeToString(sum[:]),
		})

		framesDir := configgen.FramesDir(opts, s.ID)
		if err := os.MkdirAll(framesDir, 0o755); err != nil {
			return nil, err
		}
		previewArgs := []string{
			"-file", filepath.Join(framesDir, "output.jpg"),
			"-port", strconv.Itoa(previewPort(index)),
		}
		previewSum := sha256.Sum256([]byte(strings.Join(previewArgs, "\x00")))
		specs = append(specs, supervisor.Spec{
			ID:          s.ID + previewIDSuffix,
			Bin:         previewBin,
			Args:        previewArgs,
			Env:         engineEnv,
			Fingerprint: hex.EncodeToString(previewSum[:]),
		})
	}
	return specs, nil
}

func startBroker(extDir string) (*exec.Cmd, error) {
	confPath := "/tmp/argus-watcher-mosquitto.conf"
	conf := "listener 1883 0.0.0.0\nallow_anonymous true\npersistence false\nlog_dest stdout\n"
	if err := os.WriteFile(confPath, []byte(conf), 0o644); err != nil {
		return nil, err
	}
	cmd, err := broker.Start(filepath.Join(extDir, "bin", "mosquitto"), confPath)
	if err != nil {
		return nil, err
	}
	return cmd, broker.WaitForBroker("127.0.0.1:1883", brokerTimeout)
}

// engineEnviron returns the shared library/GStreamer environment the
// engine needs, mirroring the current bsext_init setup.
func engineEnviron(extDir string) []string {
	return []string{
		"LD_LIBRARY_PATH=" + extDir + "/bin:" + extDir + "/lib:/usr/lib",
		"GST_PLUGIN_PATH=" + extDir + "/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0",
		"GST_PLUGIN_SYSTEM_PATH=" + extDir + "/lib/gstreamer-1.0:/usr/lib/gstreamer-1.0",
		"GST_PLUGIN_SCANNER=/usr/libexec/gstreamer-1.0/gst-plugin-scanner",
		"GST_REGISTRY_REUSE_PLUGIN_SCANNER=1",
		"GST_DEBUG_NO_COLOR=1",
	}
}
