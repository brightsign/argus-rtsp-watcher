// Package config loads and validates watcher.json, the operator-facing
// list of RTSP streams the supervisor serves.
package config

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"regexp"
	"strings"
)

// DefaultTargetFPS caps per-stream capture when a stream omits target_fps.
const DefaultTargetFPS = 10

// streamIDPattern validates stream IDs are filesystem/topic-safe.
var streamIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

// Stream is one camera the watcher supervises. Exactly one source must be set:
// RTSPURL, FilePath (file-driven tests), or USB (the auto-detected local USB
// camera).
type Stream struct {
	ID         string    `json:"id"`
	RTSPURL    string    `json:"rtsp_url"`
	FilePath   string    `json:"file_path"`
	USB        bool      `json:"usb"`        // use the auto-detected local USB camera
	USBWidth   int       `json:"usb_width"`  // optional USB capture width  (0 = engine default)
	USBHeight  int       `json:"usb_height"` // optional USB capture height (0 = engine default)
	USBFPS     int       `json:"usb_fps"`    // optional USB capture fps    (0 = engine default)
	TargetFPS  int       `json:"target_fps"`
	Confidence *float64  `json:"confidence"` // optional person-detection threshold 0..1 (nil = engine default 0.5)
	Zone       []float64 `json:"zone"`       // optional [x0,y0,x1,y1], normalized 0..1
}

// Watcher is the parsed watcher.json document.
type Watcher struct {
	Streams []Stream `json:"streams"`
}

// SearchPaths returns the watcher.json discovery order, first-found-wins.
// packageDefault is the fallback shipped inside the extension directory.
func SearchPaths(packageDefault string) []string {
	return []string{
		"/storage/sd/watcher.json",
		"/storage/sd/configs/watcher.json",
		"/storage/ssd/watcher.json",
		"/storage/ssd/configs/watcher.json",
		packageDefault,
	}
}

// Discover returns the first path that exists, or an error naming every
// path searched.
func Discover(paths []string) (string, error) {
	for _, p := range paths {
		if info, err := os.Stat(p); err == nil && !info.IsDir() {
			return p, nil
		}
	}
	return "", fmt.Errorf("watcher.json not found in any of: %s", strings.Join(paths, ", "))
}

// Load reads, parses, defaults, and validates a watcher.json file.
func Load(path string) (Watcher, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Watcher{}, fmt.Errorf("read %s: %w", path, err)
	}
	var w Watcher
	dec := json.NewDecoder(strings.NewReader(string(data)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&w); err != nil {
		return Watcher{}, fmt.Errorf("parse %s: %w", path, err)
	}
	for i := range w.Streams {
		if w.Streams[i].TargetFPS == 0 {
			w.Streams[i].TargetFPS = DefaultTargetFPS
		}
	}
	if err := w.Validate(); err != nil {
		return Watcher{}, err
	}
	return w, nil
}

// Validate enforces uniqueness and safety of stream identities and that
// each stream names a source.
func (w Watcher) Validate() error {
	if len(w.Streams) == 0 {
		return fmt.Errorf("watcher.json defines no streams")
	}
	seen := make(map[string]bool, len(w.Streams))
	for _, s := range w.Streams {
		if s.ID == "" {
			return fmt.Errorf("stream id must not be empty")
		}
		if !streamIDPattern.MatchString(s.ID) {
			return fmt.Errorf("stream id %q contains unsafe characters", s.ID)
		}
		if seen[s.ID] {
			return fmt.Errorf("duplicate stream id %q", s.ID)
		}
		seen[s.ID] = true
		sources := 0
		if s.RTSPURL != "" {
			sources++
		}
		if s.FilePath != "" {
			sources++
		}
		if s.USB {
			sources++
		}
		if sources != 1 {
			return fmt.Errorf("stream %q must set exactly one source (rtsp_url, file_path, or usb), got %d", s.ID, sources)
		}
		if !s.USB && (s.USBWidth != 0 || s.USBHeight != 0 || s.USBFPS != 0) {
			return fmt.Errorf("stream %q sets usb_width/usb_height/usb_fps but usb is not true", s.ID)
		}
		if s.Confidence != nil {
			c := *s.Confidence
			if math.IsNaN(c) || c < 0.0 || c > 1.0 {
				return fmt.Errorf("stream %q confidence = %g is out of range [0,1]", s.ID, c)
			}
		}
		if s.Zone != nil {
			if len(s.Zone) != 4 {
				return fmt.Errorf("stream %q zone must have 4 values [x0,y0,x1,y1], got %d", s.ID, len(s.Zone))
			}
			for i, v := range s.Zone {
				if math.IsNaN(v) {
					return fmt.Errorf("stream %q zone value %d is NaN", s.ID, i)
				}
				if v < 0.0 || v > 1.0 {
					return fmt.Errorf("stream %q zone value %d = %g is out of range [0,1]", s.ID, i, v)
				}
			}
			if s.Zone[0] >= s.Zone[2] || s.Zone[1] >= s.Zone[3] {
				return fmt.Errorf("stream %q zone must have x0<x1 and y0<y1, got [%g,%g,%g,%g]",
					s.ID, s.Zone[0], s.Zone[1], s.Zone[2], s.Zone[3])
			}
		}
	}
	return nil
}
