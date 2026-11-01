// config-server: a tiny on-player HTTP server (default :20300) that serves the
// zone dashboard and reads/writes the live config.json. An operator points a
// browser at the player, draws a detection zone on each camera's live snapshot,
// and Saves -- the write lands in the same config.json the watcher discovered,
// which re-reads it at once (SIGHUP to the watcher; the watcher's file watch is
// the fallback). A global Restart signals the watcher (SIGUSR1) to re-read its
// config files and restart mediamtx and the engines. The camera
// images for drawing come from the per-stream image-stream-server (20200+index);
// this server only owns the page + config read/write + those two signals.
//
// Supervised by the watcher (cmd/watcher) like the engines and mediamtx.
package main

import (
	_ "embed"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"syscall"
)

// restartHeaderName/Value must accompany a POST /restart. A custom header
// forces a browser to preflight a cross-origin request, which this server never
// approves, so another web page the operator has open cannot restart services.
const (
	restartHeaderName  = "X-Argus-Action"
	restartHeaderValue = "restart"
)

// errNoWatcher means this server was not started under a watcher, so there is
// nothing to signal (the file watch still applies a saved config).
var errNoWatcher = errors.New("no watcher attached (start with --watcher-pid)")

//go:embed web/index.html
var indexHTML []byte

type server struct {
	configPath string
	// signalWatcher delivers sig to the supervising watcher. It is a field so
	// tests can observe signals without ever signalling a real process.
	signalWatcher func(sig syscall.Signal) error
}

// newWatcherSignaler returns a signal sender for the watcher with the given
// pid. It refuses unless that pid is still this process's parent: the watcher
// is config-server's supervisor, so a different parent means the watcher is
// gone (and its pid could be reused by an unrelated process) or config-server
// was launched standalone.
func newWatcherSignaler(watcherPID int) func(syscall.Signal) error {
	return func(sig syscall.Signal) error {
		if watcherPID <= 0 {
			return errNoWatcher
		}
		if os.Getppid() != watcherPID {
			return fmt.Errorf("pid %d is not this process's parent", watcherPID)
		}
		return syscall.Kill(watcherPID, sig)
	}
}

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/config", s.handleConfig)
	mux.HandleFunc("/restart", s.handleRestart)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		// no-store so a reinstalled extension's page is never masked by browser cache.
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(indexHTML)
	})
	return mux
}

func (s *server) handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		b, err := os.ReadFile(s.configPath)
		if err != nil {
			http.Error(w, "config not found: "+err.Error(), http.StatusNotFound)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
	case http.MethodPut, http.MethodPost:
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
		if err != nil {
			http.Error(w, "read error", http.StatusBadRequest)
			return
		}
		// Refuse to overwrite the live config with anything that is not valid JSON.
		if !json.Valid(body) {
			http.Error(w, "invalid JSON", http.StatusBadRequest)
			return
		}
		if err := atomicWrite(s.configPath, body); err != nil {
			http.Error(w, "write error: "+err.Error(), http.StatusInternalServerError)
			return
		}
		// SIGHUP makes the watcher re-read config.json now instead of at its
		// next poll. A failed signal is not a failed save: the file is on disk
		// and the watcher's file watch applies it within a few seconds.
		reloadSignaled := true
		if err := s.signalWatcher(syscall.SIGHUP); err != nil {
			log.Printf("config saved but SIGHUP to watcher failed: %v", err)
			reloadSignaled = false
		}
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true, "reload_signaled": reloadSignaled})
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

// handleRestart asks the watcher to re-read config.json and mediamtx.yml and
// restart mediamtx and every engine (SIGUSR1). config-server itself is left
// running so this response, and the page, survive the restart.
func (s *server) handleRestart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if r.Header.Get(restartHeaderName) != restartHeaderValue {
		http.Error(w, "missing "+restartHeaderName+": "+restartHeaderValue+" header", http.StatusForbidden)
		return
	}
	if err := s.signalWatcher(syscall.SIGUSR1); err != nil {
		http.Error(w, "cannot signal watcher: "+err.Error(), http.StatusServiceUnavailable)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(body); err != nil {
		log.Printf("write response: %v", err)
	}
}

// atomicWrite writes to a temp file in the same dir then renames it over the
// target, so a crash mid-write never leaves a truncated config.
func atomicWrite(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func main() {
	port := flag.Int("port", 20300, "listen port")
	cfg := flag.String("config", "/storage/sd/configs/config.json", "config.json to serve/edit")
	watcherPID := flag.Int("watcher-pid", 0, "pid of the supervising watcher; enables the reload (SIGHUP) and restart (SIGUSR1) signals")
	flag.Parse()

	s := &server{configPath: *cfg, signalWatcher: newWatcherSignaler(*watcherPID)}
	addr := ":" + strconv.Itoa(*port)
	log.Printf("config-server listening on %s, editing %s", addr, *cfg)
	log.Fatal(http.ListenAndServe(addr, s.routes()))
}
