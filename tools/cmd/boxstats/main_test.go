package main

import (
	"testing"

	"argus-tools/internal/analytics"
)

func TestParseRegion(t *testing.T) {
	r, err := parseRegion("150,60,800,560")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r != (analytics.Region{150, 60, 800, 560}) {
		t.Fatalf("parsed %v", r)
	}
	if _, err := parseRegion("1,2,3"); err == nil {
		t.Fatal("expected error on 3-field region")
	}
	if _, err := parseRegion("800,60,150,560"); err == nil {
		t.Fatal("expected error on x0>=x1")
	}
}

func TestInsidePredicate(t *testing.T) {
	r := analytics.Region{150, 60, 800, 560}
	base, _ := insidePredicate("base", r)
	// Box whose feet (bottom-center) are at (200,500): inside; center (200,300) also inside.
	if !base(analytics.Bbox{150, 100, 250, 500}) {
		t.Fatal("base rule: standing-in-region box reported outside")
	}
	// Feet at (900,500) -> outside region horizontally.
	if base(analytics.Bbox{850, 100, 950, 500}) {
		t.Fatal("base rule: box with feet outside reported inside")
	}
	if _, err := insidePredicate("bogus", r); err == nil {
		t.Fatal("expected error on unknown mode")
	}
}

func TestPercentile(t *testing.T) {
	xs := []float64{10, 20, 30, 40, 50} // sorted
	if got := percentile(xs, 0.0); got != 10 {
		t.Fatalf("p0 = %v, want 10", got)
	}
	if got := percentile(xs, 1.0); got != 50 {
		t.Fatalf("p100 = %v, want 50", got)
	}
	if got := percentile(xs, 0.5); got != 30 {
		t.Fatalf("p50 = %v, want 30", got)
	}
	if got := percentile(xs, 0.25); got != 20 {
		t.Fatalf("p25 = %v, want 20", got)
	}
}
