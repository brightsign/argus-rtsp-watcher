// Command framebox decodes the frame_seq stamp burned into a recorded
// annotated frame (from the /video MJPEG stream) and prints the bounding-box
// data that was recorded for that exact frame over MQTT.
//
// The boxes are NOT in the image pixels; the annotated frame only has boxes
// drawn on it. framebox reads the frame_seq from the machine-readable stamp,
// then looks that frame_seq up in a recorded MQTT analytics JSONL file (see
// record-frames.sh) to produce the structured boxes.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"os"
	"time"

	"argus-tools/internal/analytics"
)

// msFormat renders epoch-milliseconds as an ISO-8601 UTC timestamp.
func msFormat(ms int64) string {
	return time.UnixMilli(ms).UTC().Format("2006-01-02T15:04:05.000Z")
}

func main() {
	boxesPath := flag.String("boxes", "", "recorded MQTT analytics JSONL (from record-frames.sh) to look up boxes for this frame")
	asJSON := flag.Bool("json", false, "emit the matched analytics message as JSON instead of a text summary")
	flag.Usage = usage
	flag.Parse()

	if flag.NArg() != 1 {
		usage()
		os.Exit(2)
	}
	imagePath := flag.Arg(0)

	seq, err := frameSeqOfFile(imagePath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "framebox: %v\n", err)
		os.Exit(1)
	}

	if *boxesPath == "" {
		fmt.Printf("frame_seq: %d\n", seq)
		fmt.Fprintln(os.Stderr, "framebox: no --boxes file given; decoded frame_seq only")
		return
	}

	msg, err := lookupBoxes(*boxesPath, seq)
	if err != nil {
		fmt.Fprintf(os.Stderr, "framebox: %v\n", err)
		os.Exit(3)
	}

	if *asJSON {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		if err := enc.Encode(msg); err != nil {
			fmt.Fprintf(os.Stderr, "framebox: %v\n", err)
			os.Exit(1)
		}
		return
	}
	printSummary(imagePath, seq, msg)
}

func frameSeqOfFile(path string) (uint64, error) {
	f, err := os.Open(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return 0, fmt.Errorf("decode %s: %w", path, err)
	}
	return decodeFrameSeq(img)
}

// lookupBoxes scans a JSONL analytics log for the message whose frame_seq
// matches seq, stopping at the first match.
func lookupBoxes(path string, seq uint64) (*analytics.Message, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	errFound := errors.New("found")
	var found *analytics.Message
	err = analytics.Scan(f, func(m *analytics.Message) error {
		if m.FrameSeq == seq {
			cp := *m
			found = &cp
			return errFound
		}
		return nil
	})
	if err != nil && !errors.Is(err, errFound) {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if found == nil {
		return nil, fmt.Errorf("no MQTT message recorded for frame_seq %d in %s (MQTT publishes ~1/s; this frame may have no boxes)", seq, path)
	}
	return found, nil
}

func printSummary(imagePath string, seq uint64, m *analytics.Message) {
	fmt.Printf("image:     %s\n", imagePath)
	fmt.Printf("frame_seq: %d\n", seq)
	fmt.Printf("frame_ts:  %d  (%s)\n", m.FrameTs, msFormat(m.FrameTs))
	fmt.Printf("stream:    %s   frame: %dx%d   people: %d\n", m.Stream, m.FrameW, m.FrameH, m.People)
	fmt.Printf("\ntracks (%d):\n", len(m.Tracks))
	if len(m.Tracks) == 0 {
		fmt.Println("  (none - empty scene)")
		return
	}
	for _, t := range m.Tracks {
		gaze := "no"
		if t.Gaze != nil && t.Gaze.Detected == 1 {
			gaze = fmt.Sprintf("yes time=%.2f face=%s", t.Gaze.Time, t.Gaze.FaceBbox)
		}
		fmt.Printf("  #%-3d bbox=%s score=%.2f  gaze=%s\n", t.ID, t.Bbox, t.Score, gaze)
	}
}

func usage() {
	fmt.Fprint(os.Stderr, `framebox - decode the frame_seq stamp from a recorded annotated frame and
print the bounding boxes recorded for that frame.

Usage:
  framebox [--boxes boxes.jsonl] [--json] <image.jpg>

Arguments:
  <image.jpg>   an annotated frame recorded from the /video MJPEG stream
                (the stamp is burned into the lower-left corner)

Flags:
`)
	flag.PrintDefaults()
	fmt.Fprint(os.Stderr, `
Examples:
  framebox frame_00000123.jpg                              # decode frame_seq only
  framebox --boxes lobby.boxes.jsonl frame_00000123.jpg   # decode + print boxes
  framebox --json --boxes lobby.boxes.jsonl frame_00000123.jpg

Exit codes: 0 ok, 1 image/decode error, 2 usage, 3 no matching MQTT message.
`)
}
