package analytics

import (
	"strings"
	"testing"
)

func TestBboxGeometry(t *testing.T) {
	b := Bbox{100, 200, 300, 700} // 200 wide, 500 tall
	if got := b.Area(); got != 100000 {
		t.Fatalf("Area = %v, want 100000", got)
	}
	if cx, cy := b.Center(); cx != 200 || cy != 450 {
		t.Fatalf("Center = %v,%v, want 200,450", cx, cy)
	}
	if bx, by := b.Base(); bx != 200 || by != 700 {
		t.Fatalf("Base = %v,%v, want 200,700", bx, by)
	}
}

func TestRegionContainment(t *testing.T) {
	r := Region{150, 60, 800, 560}
	if !r.Contains(200, 450) {
		t.Fatal("point inside reported outside")
	}
	if r.Contains(900, 450) {
		t.Fatal("point right of region reported inside")
	}
	inside := Bbox{200, 100, 300, 400}
	if !r.ContainsBox(inside) {
		t.Fatal("fully-contained box reported outside")
	}
	straddle := Bbox{100, 100, 300, 400} // x0 < region x0
	if r.ContainsBox(straddle) {
		t.Fatal("straddling box reported fully inside")
	}
}

func TestScanParsesAndStripsPrefix(t *testing.T) {
	// line 1 has a receive-timestamp prefix; line 2 is bare JSON; blank line skipped.
	data := `1790686917.200 {"stream":"entrance","frame_seq":291,"frame_w":1024,"frame_h":576,"people":1,"tracks":[{"id":7,"bbox":[100,200,300,700],"score":0.9}]}

{"stream":"entrance","frame_seq":292,"people":0,"tracks":[]}
`
	var seqs []uint64
	err := Scan(strings.NewReader(data), func(m *Message) error {
		seqs = append(seqs, m.FrameSeq)
		return nil
	})
	if err != nil {
		t.Fatalf("Scan error: %v", err)
	}
	if len(seqs) != 2 || seqs[0] != 291 || seqs[1] != 292 {
		t.Fatalf("parsed seqs = %v, want [291 292]", seqs)
	}
}

func TestScanRejectsMalformedLine(t *testing.T) {
	data := "{\"frame_seq\":1}\nnot json at all\n"
	err := Scan(strings.NewReader(data), func(*Message) error { return nil })
	if err == nil {
		t.Fatal("expected error on malformed line, got nil")
	}
	if !strings.Contains(err.Error(), "line 2") {
		t.Fatalf("error should name line 2, got: %v", err)
	}
}
