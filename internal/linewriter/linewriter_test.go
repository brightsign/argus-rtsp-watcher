package linewriter

import (
	"errors"
	"strings"
	"sync"
	"testing"
	"time"
)

// recordingWriter keeps every Write call separate, so a test can tell whether a
// line arrived whole in one call.
type recordingWriter struct {
	mu     sync.Mutex
	writes []string
	err    error
}

func (r *recordingWriter) Write(p []byte) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.err != nil {
		return 0, r.err
	}
	r.writes = append(r.writes, string(p))
	return len(p), nil
}

var fixedTime = time.Date(2026, 10, 25, 15, 2, 36, 81234000, time.UTC)

func newTestPrefixer(tag string, dst *recordingWriter) *Prefixer {
	p := NewPrefixer(tag, dst)
	p.now = func() time.Time { return fixedTime }
	return p
}

const stamp = "2026/10/25 15:02:36.081234"

func TestPrefixesEachCompleteLine(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[reo1]", dst)

	if n, err := p.Write([]byte("first\nsecond\n")); err != nil || n != 13 {
		t.Fatalf("Write = %d, %v", n, err)
	}

	want := []string{stamp + " [reo1] first\n", stamp + " [reo1] second\n"}
	if len(dst.writes) != 2 || dst.writes[0] != want[0] || dst.writes[1] != want[1] {
		t.Errorf("writes = %q, want %q", dst.writes, want)
	}
}

func TestEachLineIsASingleWrite(t *testing.T) {
	// A shared, serializing destination can only keep lines apart if every line
	// arrives in one call.
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("a\nb\nc\n"))

	if len(dst.writes) != 3 {
		t.Fatalf("got %d writes for 3 lines: %q", len(dst.writes), dst.writes)
	}
	for _, w := range dst.writes {
		if strings.Count(w, "\n") != 1 || !strings.HasSuffix(w, "\n") {
			t.Errorf("write %q is not exactly one line", w)
		}
	}
}

func TestAPartialLineWaitsForItsNewline(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("hel"))
	if len(dst.writes) != 0 {
		t.Fatalf("emitted a partial line: %q", dst.writes)
	}
	p.Write([]byte("lo wor"))
	p.Write([]byte("ld\nnext"))

	if len(dst.writes) != 1 || dst.writes[0] != stamp+" [x] hello world\n" {
		t.Errorf("writes = %q, want the one joined line", dst.writes)
	}
}

func TestFlushEmitsTheUnterminatedTail(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("crash without newline"))
	if err := p.Flush(); err != nil {
		t.Fatal(err)
	}

	if len(dst.writes) != 1 || dst.writes[0] != stamp+" [x] crash without newline\n" {
		t.Errorf("writes = %q", dst.writes)
	}
}

func TestFlushWithNothingPendingWritesNothing(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("done\n"))
	dst.writes = nil
	if err := p.Flush(); err != nil {
		t.Fatal(err)
	}
	if len(dst.writes) != 0 {
		t.Errorf("Flush wrote %q though nothing was pending", dst.writes)
	}
}

func TestBlankLinesAreKept(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("a\n\nb\n"))

	if len(dst.writes) != 3 || dst.writes[1] != stamp+" [x] \n" {
		t.Errorf("writes = %q, want the blank line preserved", dst.writes)
	}
}

func TestCarriageReturnBeforeTheNewlineIsDropped(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte("windows style\r\n"))

	if len(dst.writes) != 1 || dst.writes[0] != stamp+" [x] windows style\n" {
		t.Errorf("writes = %q", dst.writes)
	}
}

func TestAnOverlongUnterminatedLineIsEmittedRatherThanBufferedForever(t *testing.T) {
	dst := &recordingWriter{}
	p := newTestPrefixer("[x]", dst)

	p.Write([]byte(strings.Repeat("z", maxPendingBytes+1)))

	if len(dst.writes) != 1 {
		t.Fatalf("got %d writes, want the oversize line emitted once", len(dst.writes))
	}
	if !strings.HasSuffix(dst.writes[0], strings.Repeat("z", maxPendingBytes+1)+"\n") {
		t.Error("oversize line was truncated")
	}
}

func TestADestinationErrorIsReturned(t *testing.T) {
	boom := errors.New("disk full")
	dst := &recordingWriter{err: boom}
	p := newTestPrefixer("[x]", dst)

	if _, err := p.Write([]byte("line\n")); !errors.Is(err, boom) {
		t.Errorf("err = %v, want the destination's error", err)
	}
}

func TestTwoPrefixersSharingADestinationNeverMixALine(t *testing.T) {
	dst := &recordingWriter{}
	a := newTestPrefixer("[a]", dst)
	b := newTestPrefixer("[b]", dst)

	var wg sync.WaitGroup
	for _, p := range []*Prefixer{a, b} {
		wg.Add(1)
		go func(p *Prefixer) {
			defer wg.Done()
			for i := 0; i < 500; i++ {
				p.Write([]byte("some line of output\n"))
			}
		}(p)
	}
	wg.Wait()

	if len(dst.writes) != 1000 {
		t.Fatalf("got %d lines, want 1000", len(dst.writes))
	}
	for _, w := range dst.writes {
		if w != stamp+" [a] some line of output\n" && w != stamp+" [b] some line of output\n" {
			t.Fatalf("mixed or damaged line: %q", w)
		}
	}
}
