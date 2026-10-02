// Package analytics parses the analytics/v7.0 MQTT payloads recorded by
// record-frames.sh (one JSON object per line, optionally prefixed by a
// whitespace-separated receive timestamp) and provides the geometry helpers
// the recording tools share.
package analytics

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"strings"
)

// Bbox is [x0, y0, x1, y1] in the source frame's pixel space (top-left origin).
type Bbox [4]float64

// Area returns the box area in square pixels.
func (b Bbox) Area() float64 { return (b[2] - b[0]) * (b[3] - b[1]) }

// Center returns the box center point.
func (b Bbox) Center() (x, y float64) { return (b[0] + b[2]) / 2, (b[1] + b[3]) / 2 }

// Base returns the bottom-center point (a standing person's ground contact).
func (b Bbox) Base() (x, y float64) { return (b[0] + b[2]) / 2, b[3] }

func (b Bbox) String() string {
	return fmt.Sprintf("[%.0f,%.0f,%.0f,%.0f]", b[0], b[1], b[2], b[3])
}

// Region is a rectangle [x0, y0, x1, y1] in the same pixel space as Bbox.
type Region [4]float64

// Contains reports whether the point (x,y) lies within the region (inclusive).
func (r Region) Contains(x, y float64) bool {
	return x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3]
}

// ContainsBox reports whether the whole box lies within the region.
func (r Region) ContainsBox(b Bbox) bool {
	return b[0] >= r[0] && b[1] >= r[1] && b[2] <= r[2] && b[3] <= r[3]
}

type Gaze struct {
	Detected int     `json:"detected"`
	Time     float64 `json:"time"`
	FaceBbox Bbox    `json:"face_bbox"`
}

type Track struct {
	ID    int     `json:"id"`
	State string  `json:"state"`
	Bbox  Bbox    `json:"bbox"`
	Score float64 `json:"score"`
	Gaze  *Gaze   `json:"gaze"`
}

// Message is the subset of the analytics/v7.0 payload the tools consume.
type Message struct {
	Schema   string  `json:"schema"`
	Stream   string  `json:"stream"`
	Device   string  `json:"device"`
	FrameSeq uint64  `json:"frame_seq"`
	FrameTs  int64   `json:"frame_ts"`
	FrameW   int     `json:"frame_w"`
	FrameH   int     `json:"frame_h"`
	People   int     `json:"people"`
	Tracks   []Track `json:"tracks"`
}

// Scan reads a JSONL analytics log and calls fn for each message. Blank lines
// are skipped; a non-blank line that does not parse as JSON is a hard error
// (with its 1-based line number), not silently skipped.
func Scan(r io.Reader, fn func(*Message) error) error {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024) // lines grow with track count
	lineNo := 0
	for sc.Scan() {
		lineNo++
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		if i := strings.IndexByte(line, '{'); i > 0 {
			line = line[i:] // drop the optional "<recv-ts> " prefix
		}
		var msg Message
		if err := json.Unmarshal([]byte(line), &msg); err != nil {
			return fmt.Errorf("line %d: malformed analytics line: %w", lineNo, err)
		}
		if err := fn(&msg); err != nil {
			return err
		}
	}
	return sc.Err()
}
