// Command boxstats reads a recorded MQTT analytics JSONL (from
// record-frames.sh) and reports the range of person bounding-box areas for the
// boxes that fall inside a region -- e.g. "the fence area". Use it to pick a
// size band for a spatial/area filter.
//
// A box counts as "inside" the region by one of three rules (--inside):
//   base   the box's bottom-center (a standing person's ground contact) is in
//          the region (default -- best proxy for where the person stands)
//   center the box center is in the region
//   full   the entire box is within the region (strictest)
package main

import (
	"flag"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	"argus-tools/internal/analytics"
)

// defaultRegion is the entrance "whole fenced entry" area (1024x576 frame).
const defaultRegion = "150,60,800,560"

func main() {
	regionStr := flag.String("region", defaultRegion, "fence region as x0,y0,x1,y1 in frame pixels")
	insideMode := flag.String("inside", "base", "containment rule: base | center | full")
	streamFilter := flag.String("stream", "", "only count messages whose stream id contains this substring (default: all)")
	flag.Usage = usage
	flag.Parse()

	if flag.NArg() != 1 {
		usage()
		os.Exit(2)
	}
	boxesPath := flag.Arg(0)

	region, err := parseRegion(*regionStr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "boxstats: %v\n", err)
		os.Exit(2)
	}
	inside, err := insidePredicate(*insideMode, region)
	if err != nil {
		fmt.Fprintf(os.Stderr, "boxstats: %v\n", err)
		os.Exit(2)
	}

	f, err := os.Open(boxesPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "boxstats: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	var (
		messages, withPeople, totalTracks int
		framesWithInside                   int
		frameW, frameH                     int
		areas                              []float64
		widths, heights                    []float64
	)
	err = analytics.Scan(f, func(m *analytics.Message) error {
		if *streamFilter != "" && !strings.Contains(m.Stream, *streamFilter) {
			return nil
		}
		messages++
		if frameW == 0 && m.FrameW > 0 {
			frameW, frameH = m.FrameW, m.FrameH
		}
		if m.People > 0 {
			withPeople++
		}
		hitThisFrame := false
		for _, t := range m.Tracks {
			totalTracks++
			if inside(t.Bbox) {
				areas = append(areas, t.Bbox.Area())
				widths = append(widths, t.Bbox[2]-t.Bbox[0])
				heights = append(heights, t.Bbox[3]-t.Bbox[1])
				hitThisFrame = true
			}
		}
		if hitThisFrame {
			framesWithInside++
		}
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "boxstats: %s: %v\n", boxesPath, err)
		os.Exit(1)
	}

	report(boxesPath, region, *insideMode, messages, withPeople, totalTracks,
		framesWithInside, frameW, frameH, areas, widths, heights)
}

func report(path string, region analytics.Region, mode string,
	messages, withPeople, totalTracks, framesWithInside, frameW, frameH int,
	areas, widths, heights []float64) {

	fmt.Printf("%s\n", path)
	fmt.Printf("  messages: %d   with people: %d   tracks total: %d\n", messages, withPeople, totalTracks)
	fmt.Printf("  region:   [%g,%g,%g,%g]   inside rule: %s\n", region[0], region[1], region[2], region[3], mode)

	if len(areas) == 0 {
		fmt.Println("\n  no boxes fell inside the region.")
		if totalTracks == 0 {
			fmt.Println("  (there were no tracks in this recording at all -- record while someone is in frame)")
		} else {
			fmt.Println("  (tracks were present but none inside -- widen --region or try --inside center)")
		}
		return
	}

	frameArea := float64(frameW) * float64(frameH)
	sort.Float64s(areas)
	fmt.Printf("\n  boxes inside: %d   across %d frames   (frame %dx%d)\n", len(areas), framesWithInside, frameW, frameH)
	fmt.Println("\n  box area (px^2):")
	printStat("min", areas[0], frameArea)
	printStat("p5", percentile(areas, 0.05), frameArea)
	printStat("median", percentile(areas, 0.50), frameArea)
	printStat("mean", mean(areas), frameArea)
	printStat("p95", percentile(areas, 0.95), frameArea)
	printStat("max", areas[len(areas)-1], frameArea)

	sort.Float64s(widths)
	sort.Float64s(heights)
	fmt.Printf("\n  width  (px):  min %.0f  median %.0f  max %.0f\n", widths[0], percentile(widths, 0.50), widths[len(widths)-1])
	fmt.Printf("  height (px):  min %.0f  median %.0f  max %.0f\n", heights[0], percentile(heights, 0.50), heights[len(heights)-1])
}

func printStat(label string, area, frameArea float64) {
	if frameArea > 0 {
		fmt.Printf("    %-7s %10.0f   (%.2f%% of frame)\n", label, area, 100*area/frameArea)
		return
	}
	fmt.Printf("    %-7s %10.0f\n", label, area)
}

func parseRegion(s string) (analytics.Region, error) {
	parts := strings.Split(s, ",")
	if len(parts) != 4 {
		return analytics.Region{}, fmt.Errorf("region must be x0,y0,x1,y1, got %q", s)
	}
	var r analytics.Region
	for i, p := range parts {
		v, err := strconv.ParseFloat(strings.TrimSpace(p), 64)
		if err != nil {
			return analytics.Region{}, fmt.Errorf("region value %q: %w", p, err)
		}
		r[i] = v
	}
	if r[0] >= r[2] || r[1] >= r[3] {
		return analytics.Region{}, fmt.Errorf("region must have x0<x1 and y0<y1, got [%g,%g,%g,%g]", r[0], r[1], r[2], r[3])
	}
	return r, nil
}

func insidePredicate(mode string, r analytics.Region) (func(analytics.Bbox) bool, error) {
	switch mode {
	case "base":
		return func(b analytics.Bbox) bool { return r.Contains(b.Base()) }, nil
	case "center":
		return func(b analytics.Bbox) bool { return r.Contains(b.Center()) }, nil
	case "full":
		return r.ContainsBox, nil
	default:
		return nil, fmt.Errorf("--inside must be base, center, or full, got %q", mode)
	}
}

func mean(xs []float64) float64 {
	var sum float64
	for _, x := range xs {
		sum += x
	}
	return sum / float64(len(xs))
}

// percentile returns the p-quantile (p in [0,1]) of xs using linear
// interpolation between order statistics. xs must be sorted ascending.
func percentile(xs []float64, p float64) float64 {
	if len(xs) == 1 {
		return xs[0]
	}
	rank := p * float64(len(xs)-1)
	lo := int(math.Floor(rank))
	hi := int(math.Ceil(rank))
	frac := rank - float64(lo)
	return xs[lo] + frac*(xs[hi]-xs[lo])
}

func usage() {
	fmt.Fprintf(os.Stderr, `boxstats - report the range of person bounding-box areas inside a region
(e.g. "the fence area") from a recorded MQTT analytics JSONL.

Usage:
  boxstats [--region x0,y0,x1,y1] [--inside base|center|full] [--stream ID] <boxes.jsonl>

Flags:
`)
	flag.PrintDefaults()
	fmt.Fprintf(os.Stderr, `
Default region is the entrance fenced entry: %s

Example:
  boxstats --region 150,60,800,560 recordings/entrance-*/boxes.jsonl

Exit codes: 0 ok, 1 file/parse error, 2 usage.
`, defaultRegion)
}
