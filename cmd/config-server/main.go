// config-server: a tiny on-player HTTP server (default :20300) that serves the
// zone dashboard and reads/writes the live config.json. An operator points a
// browser at the player, draws a detection zone on each camera's live snapshot,
// and Saves -- the write lands in the same config.json the watcher discovered,
// which hot-reloads it within a few seconds (no restart needed). The camera
// images for drawing come from the per-stream image-stream-server (20200+index);
// this server only owns the page + config read/write.
//
// Supervised by the watcher (cmd/watcher) like the engines and mediamtx.
package main

import (
	_ "embed"
	"encoding/json"
	"flag"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
)

//go:embed web/index.html
var indexHTML []byte

type server struct{ configPath string }

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/config", s.handleConfig)
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
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`))
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
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
	flag.Parse()

	s := &server{configPath: *cfg}
	addr := ":" + strconv.Itoa(*port)
	log.Printf("config-server listening on %s, editing %s", addr, *cfg)
	log.Fatal(http.ListenAndServe(addr, s.routes()))
}
