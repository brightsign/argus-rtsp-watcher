package supervisor

import (
	"bytes"
	"strings"
	"sync"
	"testing"
	"time"
)

// syncBuffer is a bytes.Buffer safe for the copy goroutine exec starts for a
// non-file Output.
type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func TestExecLauncherSendsBothStreamsToTheSpecOutput(t *testing.T) {
	out := &syncBuffer{}
	proc, err := ExecLauncher{WorkDir: t.TempDir()}.Start(Spec{
		ID:     "x",
		Bin:    "/bin/sh",
		Args:   []string{"-c", "echo to-stdout; echo to-stderr >&2"},
		Output: out,
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := proc.Wait(); err != nil {
		t.Fatal(err)
	}

	got := out.String()
	if !strings.Contains(got, "to-stdout") || !strings.Contains(got, "to-stderr") {
		t.Errorf("output = %q, want both streams captured", got)
	}
}

func TestExecLauncherWaitReturnsEvenIfAGrandchildHoldsTheOutputOpen(t *testing.T) {
	// The shell exits at once but leaves a background sleep holding the output
	// pipe. Without a bounded drain, Wait would block until that sleep ended.
	out := &syncBuffer{}
	proc, err := ExecLauncher{WorkDir: t.TempDir()}.Start(Spec{
		ID:     "x",
		Bin:    "/bin/sh",
		Args:   []string{"-c", "sleep 5 & echo parent-done"},
		Output: out,
	})
	if err != nil {
		t.Fatal(err)
	}

	done := make(chan struct{})
	go func() {
		proc.Wait()
		close(done)
	}()

	select {
	case <-done:
	case <-time.After(outputDrainDelay * 4):
		t.Fatal("Wait did not return: a grandchild holding the output pipe blocked it")
	}
	if !strings.Contains(out.String(), "parent-done") {
		t.Errorf("output = %q", out.String())
	}
}
