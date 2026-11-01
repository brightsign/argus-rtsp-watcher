// Package linewriter turns the raw output of a child process into whole,
// attributed log lines. A child's stdout and stderr are byte streams with no
// line boundaries; written straight into a shared log file, two children's
// output would interleave mid-line and nothing would say who wrote what.
package linewriter

import (
	"bytes"
	"io"
	"sync"
	"time"
)

// timestampLayout is UTC to microseconds, matching the watcher's own log lines
// (see cmd/watcher) so main.log reads as one timeline.
const timestampLayout = "2006/01/02 15:04:05.000000"

// maxPendingBytes bounds the buffered partial line. A child that never emits a
// newline would otherwise grow the buffer without limit.
const maxPendingBytes = 64 * 1024

// Prefixer writes each complete line it receives to dst as one Write call,
// prefixed with a UTC timestamp and a fixed tag:
//
//	2026/10/25 15:02:36.081234 [reo1] <line>
//
// Because each line reaches dst in a single Write, a dst that serializes
// writes (such as rotatelog.Writer) never sees two children's lines mixed.
type Prefixer struct {
	mu      sync.Mutex
	tag     string
	dst     io.Writer
	now     func() time.Time
	pending []byte
}

// NewPrefixer returns a Prefixer that tags lines with tag (for example
// "[reo1]") and writes them to dst.
func NewPrefixer(tag string, dst io.Writer) *Prefixer {
	return &Prefixer{tag: tag, dst: dst, now: time.Now}
}

// Write buffers p and emits every complete line in it. A partial line waits for
// its newline (or for Flush), and is emitted early only if it exceeds
// maxPendingBytes.
func (p *Prefixer) Write(b []byte) (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()

	p.pending = append(p.pending, b...)
	for {
		end := bytes.IndexByte(p.pending, '\n')
		if end < 0 {
			break
		}
		line := p.pending[:end]
		p.pending = p.pending[end+1:]
		if err := p.emit(line); err != nil {
			return len(b), err
		}
	}
	if len(p.pending) > maxPendingBytes {
		line := p.pending
		p.pending = nil
		if err := p.emit(line); err != nil {
			return len(b), err
		}
	}
	return len(b), nil
}

// Flush emits any partial line left in the buffer, for use when the child has
// exited without a final newline.
func (p *Prefixer) Flush() error {
	p.mu.Lock()
	defer p.mu.Unlock()

	if len(p.pending) == 0 {
		return nil
	}
	line := p.pending
	p.pending = nil
	return p.emit(line)
}

// emit writes one line, with its prefix and a trailing newline, in one Write.
func (p *Prefixer) emit(line []byte) error {
	line = bytes.TrimSuffix(line, []byte("\r"))
	out := make([]byte, 0, len(timestampLayout)+len(p.tag)+len(line)+3)
	out = append(out, p.now().UTC().Format(timestampLayout)...)
	out = append(out, ' ')
	out = append(out, p.tag...)
	out = append(out, ' ')
	out = append(out, line...)
	out = append(out, '\n')
	_, err := p.dst.Write(out)
	return err
}
