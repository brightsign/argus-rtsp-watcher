package supervisor

import (
	"context"
	"os"
	"sync"
	"syscall"
	"testing"
	"time"
)

// fakeProcess exits when told, recording signals.
type fakeProcess struct {
	exit chan error
}

func (p *fakeProcess) Wait() error { return <-p.exit }
func (p *fakeProcess) Signal(os.Signal) error {
	select {
	case p.exit <- nil:
	default:
	}
	return nil
}

type fakeLauncher struct {
	mu     sync.Mutex
	starts map[string]int
	procs  map[string]*fakeProcess
}

func newFakeLauncher() *fakeLauncher {
	return &fakeLauncher{starts: map[string]int{}, procs: map[string]*fakeProcess{}}
}

func (l *fakeLauncher) Start(s Spec) (Process, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.starts[s.ID]++
	p := &fakeProcess{exit: make(chan error, 1)}
	l.procs[s.ID] = p
	return p, nil
}

func (l *fakeLauncher) crash(id string) {
	l.mu.Lock()
	p := l.procs[id]
	l.mu.Unlock()
	p.exit <- os.ErrClosed
}

func (l *fakeLauncher) startCount(id string) int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.starts[id]
}

func TestSupervisorRestartsCrashedChildOnly(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx, []Spec{{ID: "a", Bin: "e"}, {ID: "b", Bin: "e"}})

	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })

	l.crash("a")
	waitFor(t, func() bool { return l.startCount("a") == 2 })

	if l.startCount("b") != 1 {
		t.Errorf("stream b restarted unexpectedly: %d", l.startCount("b"))
	}
	var restarts int
	for _, st := range s.Status() {
		if st.ID == "a" {
			restarts = st.Restarts
		}
	}
	if restarts < 1 {
		t.Errorf("expected a.Restarts>=1, got %d", restarts)
	}
}

func hasStream(s *Supervisor, id string) bool {
	for _, st := range s.Status() {
		if st.ID == id {
			return true
		}
	}
	return false
}

func TestReconcileAddsStreamWithoutTouchingOthers(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx, []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return l.startCount("a") == 1 })

	s.Reconcile([]Spec{{ID: "a", Bin: "e", Fingerprint: "1"}, {ID: "b", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return l.startCount("b") == 1 })

	if l.startCount("a") != 1 {
		t.Errorf("stream a was disturbed by adding b: starts=%d", l.startCount("a"))
	}
}

func TestReconcileRemovesStream(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx, []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}, {ID: "b", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })

	s.Reconcile([]Spec{{ID: "a", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return !hasStream(s, "b") })

	if !hasStream(s, "a") {
		t.Errorf("stream a should still be present")
	}
}

func TestReconcileRestartsOnFingerprintChange(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx, []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return l.startCount("a") == 1 })

	s.Reconcile([]Spec{{ID: "a", Bin: "e", Fingerprint: "2"}})
	waitFor(t, func() bool { return l.startCount("a") == 2 })
}

func TestReconcileAndRestartRestartsOnlyChosenStreams(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	specs := []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}, {ID: "b", Bin: "e", Fingerprint: "1"}}
	go s.Run(ctx, specs)
	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })

	// Same fingerprints: only the predicate can cause a restart.
	s.ReconcileAndRestart(specs, func(id string) bool { return id == "a" })
	waitFor(t, func() bool { return l.startCount("a") == 2 })

	if l.startCount("b") != 1 {
		t.Errorf("stream b was restarted though not selected: starts=%d", l.startCount("b"))
	}
}

func TestReconcileAndRestartCanRestartEveryStream(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	specs := []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}, {ID: "b", Bin: "e", Fingerprint: "1"}}
	go s.Run(ctx, specs)
	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })

	s.ReconcileAndRestart(specs, func(string) bool { return true })
	waitFor(t, func() bool { return l.startCount("a") == 2 && l.startCount("b") == 2 })
}

func TestReconcileAndRestartStillRemovesStreamsDroppedFromSpecs(t *testing.T) {
	l := newFakeLauncher()
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.Run(ctx, []Spec{{ID: "a", Bin: "e", Fingerprint: "1"}, {ID: "b", Bin: "e", Fingerprint: "1"}})
	waitFor(t, func() bool { return l.startCount("a") == 1 && l.startCount("b") == 1 })

	// b is gone from the desired set: it must be removed, not restarted.
	s.ReconcileAndRestart([]Spec{{ID: "a", Bin: "e", Fingerprint: "1"}}, func(string) bool { return true })
	waitFor(t, func() bool { return l.startCount("a") == 2 && !hasStream(s, "b") })

	if l.startCount("b") != 1 {
		t.Errorf("removed stream b was restarted: starts=%d", l.startCount("b"))
	}
}

// stubbornProcess ignores SIGTERM and only exits once it receives SIGKILL,
// so tests can exercise the grace-timeout escalation path in stopProcess.
type stubbornProcess struct {
	exit chan error

	mu      sync.Mutex
	signals []os.Signal
}

func (p *stubbornProcess) Wait() error { return <-p.exit }

func (p *stubbornProcess) Signal(sig os.Signal) error {
	p.mu.Lock()
	p.signals = append(p.signals, sig)
	p.mu.Unlock()
	if sig == syscall.SIGKILL {
		select {
		case p.exit <- nil:
		default:
		}
	}
	return nil
}

func (p *stubbornProcess) receivedSignal(sig os.Signal) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	for _, s := range p.signals {
		if s == sig {
			return true
		}
	}
	return false
}

// singleProcLauncher always hands out the same process, so a test can hold
// a direct reference to it and inspect the signals it received.
type singleProcLauncher struct {
	proc *stubbornProcess

	mu     sync.Mutex
	starts int
}

func (l *singleProcLauncher) Start(Spec) (Process, error) {
	l.mu.Lock()
	l.starts++
	l.mu.Unlock()
	return l.proc, nil
}

func (l *singleProcLauncher) startCount() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.starts
}

func TestStopProcessEscalatesToSIGKILLAfterGrace(t *testing.T) {
	proc := &stubbornProcess{exit: make(chan error, 1)}
	l := &singleProcLauncher{proc: proc}
	s := New(l, Backoff{Min: time.Millisecond, Max: 2 * time.Millisecond})
	s.shutdownGrace = 20 * time.Millisecond

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		s.Run(ctx, []Spec{{ID: "a", Bin: "e"}})
		close(done)
	}()

	waitFor(t, func() bool { return l.startCount() == 1 })

	cancel()

	waitFor(t, func() bool { return proc.receivedSignal(syscall.SIGTERM) })
	waitFor(t, func() bool { return proc.receivedSignal(syscall.SIGKILL) })
	waitFor(t, func() bool {
		select {
		case <-done:
			return true
		default:
			return false
		}
	})
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatal("condition not met before deadline")
}
