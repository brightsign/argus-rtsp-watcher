// Package config loads and validates config.json, the single operator-facing
// document: the list of RTSP streams the supervisor serves, the optional
// MediaMTX proxy toggle, and the engine template rendered per stream.
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

// MaxPreviewStride bounds preview_stride: beyond it the preview would update
// less than once in a few seconds at ordinary frame rates, which is a stalled
// picture, not a lighter one.
const MaxPreviewStride = 30

// LogLevels is the set of engine logging verbosities config.json may request
// (case-insensitive), kept in sync with the engine's parser in main.cpp. An
// empty log_level inherits the engine template's own value (packaged default
// "info"). Raising this to "warn" silences the engine's per-event INFO logs.
var LogLevels = map[string]bool{
	"debug": true, "info": true, "warn": true, "warning": true,
	"error": true, "critical": true,
}

// Zone membership modes for the per-track in_zone field. ZoneModeAny (any part
// of the bounding box overlapping the zone) is the default; ZoneModeBase
// requires the box's feet (bottom-center) to be inside the zone.
const (
	ZoneModeAny  = "any"
	ZoneModeBase = "base"
)

// DefaultZoneColor is the zone outline color drawn on the annotated frame when a
// stream omits zone_color (white).
const DefaultZoneColor = "#ffffff"

// Zone outline thickness in pixels. DefaultZoneThickness is used when a stream
// omits zone_thickness; MaxZoneThickness caps it at a moderate width.
const (
	DefaultZoneThickness = 3
	MaxZoneThickness     = 10
)

// Red in-zone alert-border width (px), independent of the zone outline width.
// DefaultZoneBorderThickness is used when a stream omits zone_border_thickness;
// MaxZoneBorderThickness caps it (the border can be much bolder than the outline).
const (
	DefaultZoneBorderThickness = 20
	MaxZoneBorderThickness     = 100
)

// Rolling window (minutes) for the on-frame zone-detect counter. Used when a
// stream omits zone_window_minutes; MaxZoneWindowMinutes caps it at 24 hours.
const (
	DefaultZoneWindowMinutes = 15
	MaxZoneWindowMinutes     = 1440
)

// Light/dark detection defaults. The spec (section 4.5) is the single source of
// truth; these values are intentionally duplicated in the C++ engine's LightCfg
// so the engine runs correctly with no light block, and must be kept in sync.
const (
	DefaultLightEnabled         = true
	DefaultLightIntervalSeconds = 10
	DefaultLightDarkLuma        = 40
	DefaultLightLightLuma       = 70
	DefaultLightChromaThreshold = 10
	DefaultLightEMAAlpha        = 0.1
	DefaultLightMinDwellSeconds = 5
	DefaultLightWarmupSeconds   = 3
	DefaultLightROIFrac         = 0.6
	DefaultLightSampleStride    = 8
	DefaultLightMonochrome      = false
	DefaultLightOnChange        = true

	// DefaultLightRegionThreshold is the region brightness-level threshold
	// (0..100, the published level scale) below which an operator-drawn light
	// region reads DARK, when the stream sets a region but omits region_threshold.
	DefaultLightRegionThreshold = 20

	// MaxLightLevel bounds the 8-bit luma and chroma thresholds.
	MaxLightLevel = 255

	// MaxLightRegionThreshold bounds the region brightness-level threshold.
	MaxLightRegionThreshold = 100
)

// DefaultConfidence is the person-detection confidence a stream gets when it
// sets no confidence of its own and the top-level global confidence is also
// unset. The supervisor fills every stream's confidence so the engine always
// receives an explicit value.
const DefaultConfidence = 0.4

// LightConfig is the optional per-stream light/dark detection block. Enabled
// is a pointer so an omitted value (default true) differs from an explicit
// false; the numeric fields are pointers for the same reason, so an explicit 0
// is validated (or honored) rather than silently defaulted.
type LightConfig struct {
	Enabled          *bool    `json:"enabled"`
	IntervalSeconds  *int     `json:"interval_seconds"`
	OnChange         *bool    `json:"on_change"`
	DarkLuma         *float64 `json:"dark_luma"`
	LightLuma        *float64 `json:"light_luma"`
	ChromaThreshold  *float64 `json:"chroma_threshold"`
	EMAAlpha         *float64 `json:"ema_alpha"`
	MinDwellSeconds  *int     `json:"min_dwell_seconds"`
	WarmupSeconds    *int     `json:"warmup_seconds"`
	ROIFrac          *float64 `json:"roi_frac"`
	SampleStride     *int     `json:"sample_stride"`
	MonochromeCamera bool     `json:"monochrome_camera"`
	// Region is an optional operator-drawn [x0,y0,x1,y1] rectangle (normalized
	// 0..1), drawn on the config-server dashboard like a detection zone. When
	// set, the analyzer measures mean luminance inside it and reads DARK when
	// that mean is below RegionThreshold, bypassing the IR/chroma detector.
	Region []float64 `json:"region"`
	// RegionThreshold is the region brightness-level threshold (0..100, the
	// published level scale); nil defaults to DefaultLightRegionThreshold.
	// Adjustable per stream like confidence.
	RegionThreshold *float64 `json:"region_threshold"`
}

// validateLight checks every operator-supplied (non-nil) value; nil fields take
// their defaults, so the dark/light ordering is checked on effective values.
func validateLight(streamID string, l *LightConfig) error {
	if l.IntervalSeconds != nil && *l.IntervalSeconds < 1 {
		return fmt.Errorf("stream %q light.interval_seconds = %d must be >= 1", streamID, *l.IntervalSeconds)
	}
	if l.SampleStride != nil && *l.SampleStride < 1 {
		return fmt.Errorf("stream %q light.sample_stride = %d must be >= 1", streamID, *l.SampleStride)
	}
	if l.EMAAlpha != nil && (*l.EMAAlpha <= 0 || *l.EMAAlpha > 1) {
		return fmt.Errorf("stream %q light.ema_alpha = %g must be > 0 and <= 1", streamID, *l.EMAAlpha)
	}
	if l.ROIFrac != nil && (*l.ROIFrac <= 0 || *l.ROIFrac > 1) {
		return fmt.Errorf("stream %q light.roi_frac = %g must be > 0 and <= 1", streamID, *l.ROIFrac)
	}
	if l.MinDwellSeconds != nil && *l.MinDwellSeconds < 0 {
		return fmt.Errorf("stream %q light.min_dwell_seconds = %d must be >= 0", streamID, *l.MinDwellSeconds)
	}
	if l.WarmupSeconds != nil && *l.WarmupSeconds < 0 {
		return fmt.Errorf("stream %q light.warmup_seconds = %d must be >= 0", streamID, *l.WarmupSeconds)
	}
	levels := []struct {
		name  string
		value *float64
	}{
		{"dark_luma", l.DarkLuma}, {"light_luma", l.LightLuma}, {"chroma_threshold", l.ChromaThreshold},
	}
	for _, level := range levels {
		if level.value != nil && (*level.value < 0 || *level.value > MaxLightLevel) {
			return fmt.Errorf("stream %q light.%s = %g is out of range [0,%d]", streamID, level.name, *level.value, MaxLightLevel)
		}
	}
	dark, light := float64(DefaultLightDarkLuma), float64(DefaultLightLightLuma)
	if l.DarkLuma != nil {
		dark = *l.DarkLuma
	}
	if l.LightLuma != nil {
		light = *l.LightLuma
	}
	if dark > light {
		return fmt.Errorf("stream %q light.dark_luma = %g must be <= light_luma = %g", streamID, dark, light)
	}
	if l.RegionThreshold != nil {
		t := *l.RegionThreshold
		if math.IsNaN(t) || t < 0 || t > MaxLightRegionThreshold {
			return fmt.Errorf("stream %q light.region_threshold = %g is out of range [0,%d]", streamID, t, MaxLightRegionThreshold)
		}
	}
	if l.Region != nil {
		if len(l.Region) != 4 {
			return fmt.Errorf("stream %q light.region must have 4 values [x0,y0,x1,y1], got %d", streamID, len(l.Region))
		}
		for i, v := range l.Region {
			if math.IsNaN(v) || v < 0 || v > 1 {
				return fmt.Errorf("stream %q light.region[%d] = %g must be in [0,1]", streamID, i, v)
			}
		}
		if l.Region[2] <= l.Region[0] || l.Region[3] <= l.Region[1] {
			return fmt.Errorf("stream %q light.region must have x1>x0 and y1>y0", streamID)
		}
	}
	return nil
}

// zoneColorPattern validates zone_color as a "#RRGGBB" hex triplet.
var zoneColorPattern = regexp.MustCompile(`^#[0-9A-Fa-f]{6}$`)

// streamIDPattern validates stream IDs are filesystem/topic-safe.
var streamIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]+$`)

// Stream is one camera the watcher supervises. Exactly one source must be set:
// RTSPURL, FilePath (file-driven tests), or USB (the auto-detected local USB
// camera).
type Stream struct {
	ID                  string       `json:"id"`
	RTSPURL             string       `json:"rtsp_url"`
	FilePath            string       `json:"file_path"`
	USB                 bool         `json:"usb"`        // use the auto-detected local USB camera
	USBWidth            int          `json:"usb_width"`  // optional USB capture width  (0 = engine default)
	USBHeight           int          `json:"usb_height"` // optional USB capture height (0 = engine default)
	USBFPS              int          `json:"usb_fps"`    // optional USB capture fps    (0 = engine default)
	TargetFPS           int          `json:"target_fps"`
	Confidence          *float64     `json:"confidence"`            // optional per-stream person-detection threshold 0..1; nil inherits the global confidence (else DefaultConfidence)
	DetectFaces         *bool        `json:"detect_faces"`          // optional face-detection toggle (nil = engine default on); false disables RetinaFace (test_yolo_only), leaving person detection running
	Zone                []float64    `json:"zone"`                  // optional [x0,y0,x1,y1], normalized 0..1
	ZoneMode            string       `json:"zone_mode"`             // zone membership test: "any" (default) or "base"
	ZoneColor           string       `json:"zone_color"`            // zone outline color "#RRGGBB" (default DefaultZoneColor)
	ZoneThickness       int          `json:"zone_thickness"`        // zone outline width in px, 1..MaxZoneThickness (default 3)
	ZoneBorderThickness int          `json:"zone_border_thickness"` // red alert-border width in px, 1..MaxZoneBorderThickness (default 20)
	ZoneWindowMin       int          `json:"zone_window_minutes"`   // rolling window (min) for the on-frame detect counter (default 15)
	Light               *LightConfig `json:"light"`                 // optional light/dark detection overrides (nil = all defaults)
}

// Mediamtx optionally runs a bundled MediaMTX instance alongside the engines,
// supervised by the watcher. Its purpose is to proxy secure (rtsps) cameras --
// e.g. UniFi Protect -- down to plain RTSP the engine can consume. Disabled
// unless Enabled is true, so existing deployments are unaffected.
type Mediamtx struct {
	Enabled bool   `json:"enabled"`
	Config  string `json:"config"` // optional explicit mediamtx.yml path (overrides discovery)
}

// Watcher is the parsed config.json document. Engine holds the per-stream engine
// template verbatim (rendered per stream by internal/configgen); it is optional,
// and when omitted the packaged default's engine block is used.
type Watcher struct {
	Streams  []Stream        `json:"streams"`
	Mediamtx Mediamtx        `json:"mediamtx"`
	Engine   json.RawMessage `json:"engine"`
	LogLevel string          `json:"log_level"` // optional global engine log verbosity; empty inherits the template default ("info")
	// Confidence is the global person-detection threshold 0..1 applied to any
	// stream that sets no confidence of its own. Nil means use DefaultConfidence.
	Confidence *float64 `json:"confidence"`
	// PreviewStride saves one preview frame in every N processed frames (1 = every
	// frame). Nil inherits the engine default, every frame. Higher values trade a
	// choppier preview for less encoding work.
	PreviewStride *int `json:"preview_stride"`
}

// MediamtxSearchPaths returns the mediamtx.yml discovery order, first-found-wins.
// packageDefault is the fallback shipped inside the extension directory.
func MediamtxSearchPaths(packageDefault string) []string {
	return []string{
		"/storage/sd/configs/mediamtx.yml",
		"/storage/sd/mediamtx.yml",
		"/storage/ssd/configs/mediamtx.yml",
		"/storage/ssd/mediamtx.yml",
		packageDefault,
	}
}

// ResolveMediamtxConfig returns the mediamtx.yml to use: the explicit override
// when set (and present), else the first existing discovery path. Lets operators
// drop a tuned mediamtx.yml on the SD card without reinstalling the extension.
func ResolveMediamtxConfig(explicit, packageDefault string) (string, error) {
	if explicit != "" {
		if info, err := os.Stat(explicit); err == nil && !info.IsDir() {
			return explicit, nil
		}
		return "", fmt.Errorf("mediamtx config %q not found", explicit)
	}
	paths := MediamtxSearchPaths(packageDefault)
	for _, p := range paths {
		if info, err := os.Stat(p); err == nil && !info.IsDir() {
			return p, nil
		}
	}
	return "", fmt.Errorf("mediamtx.yml not found in any of: %s", strings.Join(paths, ", "))
}

// ConfigSearchPaths returns the config.json discovery order, first-found-wins.
// The standard operator location is /storage/sd/configs/config.json;
// packageDefault is the fallback shipped inside the extension directory.
func ConfigSearchPaths(packageDefault string) []string {
	return []string{
		"/storage/sd/configs/config.json",
		"/storage/sd/config.json",
		"/storage/ssd/configs/config.json",
		"/storage/ssd/config.json",
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
	return "", fmt.Errorf("config.json not found in any of: %s", strings.Join(paths, ", "))
}

// EngineTemplate returns the engine template to render per stream: the loaded
// document's own "engine" block when present, otherwise the "engine" block of
// the packaged default config.json. This lets a minimal operator config carry
// only streams (+ mediamtx) and inherit the packaged engine defaults.
func EngineTemplate(w Watcher, packageDefault string) (json.RawMessage, error) {
	if len(w.Engine) > 0 {
		return w.Engine, nil
	}
	data, err := os.ReadFile(packageDefault)
	if err != nil {
		return nil, fmt.Errorf("engine block omitted and packaged default unreadable (%s): %w", packageDefault, err)
	}
	var pkg Watcher
	if err := json.Unmarshal(data, &pkg); err != nil {
		return nil, fmt.Errorf("parse packaged default %s: %w", packageDefault, err)
	}
	if len(pkg.Engine) == 0 {
		return nil, fmt.Errorf("no engine block in config and none in packaged default %s", packageDefault)
	}
	return pkg.Engine, nil
}

// DefaultLogDir is where the engines write their per-stream logs, and the watcher
// its main log, when the engine template names no log_dir.
const DefaultLogDir = "/tmp"

// EngineLogDir returns the directory log files go to: the engine template's
// "log_dir", or DefaultLogDir when it is absent or empty. The watcher puts its
// main log beside the engines' stream logs so all of them are found in one place.
func EngineLogDir(template json.RawMessage) (string, error) {
	var parsed struct {
		LogDir string `json:"log_dir"`
	}
	if err := json.Unmarshal(template, &parsed); err != nil {
		return "", fmt.Errorf("read log_dir from engine template: %w", err)
	}
	if parsed.LogDir == "" {
		return DefaultLogDir, nil
	}
	return parsed.LogDir, nil
}

// Load reads, parses, defaults, and validates a config.json file.
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
		if w.Streams[i].ZoneMode == "" {
			w.Streams[i].ZoneMode = ZoneModeAny
		}
		if w.Streams[i].ZoneColor == "" {
			w.Streams[i].ZoneColor = DefaultZoneColor
		}
		if w.Streams[i].ZoneThickness == 0 {
			w.Streams[i].ZoneThickness = DefaultZoneThickness
		}
		if w.Streams[i].ZoneBorderThickness == 0 {
			w.Streams[i].ZoneBorderThickness = DefaultZoneBorderThickness
		}
		if w.Streams[i].ZoneWindowMin == 0 {
			w.Streams[i].ZoneWindowMin = DefaultZoneWindowMinutes
		}
		// Fill the per-stream confidence so the engine always receives an explicit
		// value: an omitted stream confidence inherits the global confidence, and
		// an omitted global falls to DefaultConfidence.
		if w.Streams[i].Confidence == nil {
			c := float64(DefaultConfidence)
			if w.Confidence != nil {
				c = *w.Confidence
			}
			w.Streams[i].Confidence = &c
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
		return fmt.Errorf("config.json defines no streams")
	}
	if w.LogLevel != "" && !LogLevels[strings.ToLower(w.LogLevel)] {
		return fmt.Errorf("log_level = %q must be one of debug, info, warn, error, critical", w.LogLevel)
	}
	if w.PreviewStride != nil && (*w.PreviewStride < 1 || *w.PreviewStride > MaxPreviewStride) {
		return fmt.Errorf("preview_stride = %d is out of range [1,%d]", *w.PreviewStride, MaxPreviewStride)
	}
	if w.Confidence != nil {
		c := *w.Confidence
		if math.IsNaN(c) || c < 0.0 || c > 1.0 {
			return fmt.Errorf("global confidence = %g is out of range [0,1]", c)
		}
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
		if s.ZoneMode != "" && s.ZoneMode != ZoneModeAny && s.ZoneMode != ZoneModeBase {
			return fmt.Errorf("stream %q zone_mode = %q must be %q or %q", s.ID, s.ZoneMode, ZoneModeAny, ZoneModeBase)
		}
		if s.ZoneColor != "" && !zoneColorPattern.MatchString(s.ZoneColor) {
			return fmt.Errorf("stream %q zone_color = %q must be a %q hex color", s.ID, s.ZoneColor, "#RRGGBB")
		}
		if s.ZoneThickness != 0 && (s.ZoneThickness < 1 || s.ZoneThickness > MaxZoneThickness) {
			return fmt.Errorf("stream %q zone_thickness = %d is out of range [1,%d]", s.ID, s.ZoneThickness, MaxZoneThickness)
		}
		if s.ZoneBorderThickness != 0 && (s.ZoneBorderThickness < 1 || s.ZoneBorderThickness > MaxZoneBorderThickness) {
			return fmt.Errorf("stream %q zone_border_thickness = %d is out of range [1,%d]", s.ID, s.ZoneBorderThickness, MaxZoneBorderThickness)
		}
		if s.ZoneWindowMin != 0 && (s.ZoneWindowMin < 1 || s.ZoneWindowMin > MaxZoneWindowMinutes) {
			return fmt.Errorf("stream %q zone_window_minutes = %d is out of range [1,%d]", s.ID, s.ZoneWindowMin, MaxZoneWindowMinutes)
		}
		if s.Light != nil {
			if err := validateLight(s.ID, s.Light); err != nil {
				return err
			}
		}
	}
	return nil
}
