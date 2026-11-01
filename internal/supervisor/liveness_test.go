package supervisor

import (
	"context"
	"sync"
	"testing"
	"time"
)

func TestLivenessNeverStallsDuringStartupGrace(t *testing.T) {
	start := time.Unix(1000, 0)
	st := livenessState{startedAt: start}
	// No output at all, but still inside the 90s startup grace.
	if st.stalled(start.Add(80*time.Second), time.Time{}, 90*time.Second, 30*time.Second) {
		t.Fatal("stalled during startup grace; a process still opening its stream must not be killed")
	}
}

func TestLivenessStallsWhenNoOutputAfterGrace(t *testing.T) {
	start := time.Unix(1000, 0)
	st := livenessState{startedAt: start}
	// Past the startup grace with no output ever produced.
	now := start.Add(90*time.Second + 31*time.Second)
	if !st.stalled(now, time.Time{}, 90*time.Second, 30*time.Second) {
		t.Fatal("expected stalled when no output was ever produced past the grace")
	}
}

func TestLivenessNotStalledWhileOutputAdvances(t *testing.T) {
	start := time.Unix(1000, 0)
	st := livenessState{startedAt: start}
	grace, timeout := 10*time.Second, 30*time.Second
	// A frame every 5s, well within the timeout, for two minutes.
	for i := 1; i <= 24; i++ {
		now := start.Add(time.Duration(i) * 5 * time.Second)
		mtime := now // the engine just rewrote the frame
		if st.stalled(now, mtime, grace, timeout) {
			t.Fatalf("stalled while output was advancing at tick %d", i)
		}
	}
}

func TestLivenessStallsAfterOutputStops(t *testing.T) {
	start := time.Unix(1000, 0)
	st := livenessState{startedAt: start}
	grace, timeout := 10*time.Second, 30*time.Second

	// Healthy for a while: frames keep arriving.
	lastFrame := start.Add(60 * time.Second)
	for now := start.Add(5 * time.Second); !now.After(lastFrame); now = now.Add(5 * time.Second) {
		if st.stalled(now, now, grace, timeout) {
			t.Fatalf("stalled while healthy at %v", now)
		}
	}
	// Output now freezes at lastFrame; mtime stops advancing.
	if st.stalled(lastFrame.Add(29*time.Second), lastFrame, grace, timeout) {
		t.Fatal("stalled before the timeout elapsed after output stopped")
	}
	if !st.stalled(lastFrame.Add(31*time.Second), lastFrame, grace, timeout) {
		t.Fatal("expected stalled once output had frozen for longer than the timeout")
	}
}

func TestLivenessRecoversWhenOutputResumes(t *testing.T) {
	start := time.Unix(1000, 0)
	st := livenessState{startedAt: start}
	grace, timeout := 10*time.Second, 30*time.Second

	frozen := start.Add(40 * time.Second)
	// A long gap that has not yet crossed the timeout.
	if st.stalled(frozen.Add(20*time.Second), frozen, grace, timeout) {
		t.Fatal("stalled before the timeout")
	}
	// A fresh frame arrives: progress resets, so a later tick is healthy again.
	resumed := frozen.Add(25 * time.Second)
	if st.stalled(resumed, resumed, grace, timeout) {
		t.Fatal("stalled right after a fresh frame resumed output")
	}
	if st.stalled(resumed.Add(29*time.Second), resumed, grace, timeout) {
		t.Fatal("stalled before the timeout elapsed since output resumed")
	}
}

// statStub returns a controllable mtime for the liveness watchdog.
type statStub struct {
	mu    sync.Mutex
	mtime time.Time
	ok    bool
}

func (s *statStub) stat(string) (time.Time, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.mtime, s.ok
}

func (s *statStub) set(m time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.mtime, s.ok = m, true
}

// TestSupervisorKillsAndRestartsHungProcess exercises the full watchdog wiring:
// a running process whose output never advances is SIGKILLed and restarted,
// while a process without a LivenessFile is left alone.
func TestSupervisorKillsAndRestartsHungProcess(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	stub := &statStub{}
	s.statMtime = stub.stat
	s.livenessInterval = time.Millisecond
	s.livenessStartupGrace = 0
	s.livenessTimeout = 5 * time.Millisecond

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	// "a" is watched and never produces output; "b" has no LivenessFile.
	go s.Run(ctx, []Spec{
		{ID: "a", Bin: "e", LivenessFile: "/frames/a/output.jpg"},
		{ID: "b", Bin: "e"},
	})

	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })
	// The hung watched process gets killed and restarted.
	waitFor(t, func() bool { return l.startCount("a") >= 2 })
	// The unwatched process is never touched by the watchdog.
	if l.startCount("b") != 1 {
		t.Fatalf("unwatched process b was restarted (start count %d); watchdog must ignore specs without a LivenessFile", l.startCount("b"))
	}
}
