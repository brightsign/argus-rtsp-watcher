// Package supervisor launches, restarts, and live-reconciles one engine
// process per stream, isolating each so a single failure or config change
// never affects the others.
package supervisor

import (
	"context"
	"io"
	"os"
	"sync"
	"syscall"
	"time"
)

// defaultShutdownGrace is how long a child gets to exit on SIGTERM before
// the supervisor escalates to SIGKILL.
const defaultShutdownGrace = 5 * time.Second

// Spec describes one process to run. Fingerprint identifies the process's
// effective configuration; when it changes, the process is restarted.
type Spec struct {
	ID          string
	Bin         string
	Args        []string
	Env         []string
	Fingerprint string
	// Output receives the child's stdout and stderr. Nil inherits the
	// watcher's own. It is not part of the process's effective configuration,
	// so it never affects Fingerprint or causes a restart.
	Output io.Writer
}

// Process is a running child the supervisor can wait on and signal.
type Process interface {
	Wait() error
	Signal(os.Signal) error
}

// Launcher starts a Process from a Spec.
type Launcher interface {
	Start(Spec) (Process, error)
}

// State is a stream's supervision state.
type State string

const (
	Running    State = "running"
	Restarting State = "restarting"
	Failed     State = "failed"
)

// StreamStatus is a point-in-time view of one supervised stream.
type StreamStatus struct {
	ID       string
	State    State
	Restarts int
}

// streamHandle tracks one running stream's cancel and completion.
type streamHandle struct {
	spec   Spec
	cancel context.CancelFunc
	done   chan struct{}
}

// Supervisor runs a dynamic set of streams, each restarted independently.
type Supervisor struct {
	launcher      Launcher
	backoff       Backoff
	shutdownGrace time.Duration

	// reconcileMu serializes Reconcile and shutdown so two callers (for
	// example a config-file watch and a SIGHUP handler) can never both
	// start an engine for the same stream id at once.
	reconcileMu sync.Mutex

	mu      sync.Mutex
	parent  context.Context
	running map[string]*streamHandle
	status  map[string]*StreamStatus
}

// New builds a Supervisor.
func New(l Launcher, b Backoff) *Supervisor {
	return &Supervisor{
		launcher:      l,
		backoff:       b,
		shutdownGrace: defaultShutdownGrace,
		running:       map[string]*streamHandle{},
		status:        map[string]*StreamStatus{},
	}
}

// Run applies initial, then blocks until ctx is cancelled, at which point
// it stops all streams and returns. Reconcile may be called concurrently
// while Run is blocked.
func (s *Supervisor) Run(ctx context.Context, initial []Spec) {
	s.mu.Lock()
	s.parent = ctx
	s.mu.Unlock()
	s.Reconcile(initial)
	<-ctx.Done()
	s.shutdown()
}

// Reconcile drives the running set toward specs: starts new streams, stops
// removed streams, and restarts streams whose Fingerprint changed.
// Untouched streams keep running.
func (s *Supervisor) Reconcile(specs []Spec) {
	s.reconcile(specs, nil)
}

// ReconcileAndRestart is Reconcile that also restarts every running stream
// for which shouldRestart returns true, even when its Fingerprint is
// unchanged. It lets an operator force a clean restart of processes whose
// inputs live outside their spec (for example a mediamtx.yml edited in place).
func (s *Supervisor) ReconcileAndRestart(specs []Spec, shouldRestart func(id string) bool) {
	s.reconcile(specs, shouldRestart)
}

func (s *Supervisor) reconcile(specs []Spec, shouldRestart func(id string) bool) {
	s.reconcileMu.Lock()
	defer s.reconcileMu.Unlock()

	desired := make(map[string]Spec, len(specs))
	for _, sp := range specs {
		desired[sp.ID] = sp
	}

	type stopping struct {
		id      string
		handle  *streamHandle
		removed bool
	}

	s.mu.Lock()
	parent := s.parent
	var toStop []stopping
	for id, h := range s.running {
		d, ok := desired[id]
		forced := ok && shouldRestart != nil && shouldRestart(id)
		if !ok || forced || d.Fingerprint != h.spec.Fingerprint {
			toStop = append(toStop, stopping{id: id, handle: h, removed: !ok})
			delete(s.running, id)
		}
	}
	s.mu.Unlock()

	// Wait for stopped streams to fully exit before starting replacements,
	// so two engines never share a stream id, config path, or GST registry.
	// A removed stream's status entry is cleared only once its goroutine
	// has fully exited, so a record() call still in flight cannot recreate
	// the entry after it is deleted.
	// Cancel every stopping stream before waiting on any, so their shutdown
	// grace periods overlap instead of adding up across a global restart.
	for _, st := range toStop {
		st.handle.cancel()
	}
	for _, st := range toStop {
		<-st.handle.done
		if st.removed {
			s.mu.Lock()
			delete(s.status, st.id)
			s.mu.Unlock()
		}
	}

	if parent == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, sp := range desired {
		if _, ok := s.running[id]; ok {
			continue
		}
		ctx, cancel := context.WithCancel(parent)
		h := &streamHandle{spec: sp, cancel: cancel, done: make(chan struct{})}
		s.running[id] = h
		s.status[id] = &StreamStatus{ID: id, State: Restarting}
		go s.superviseOne(ctx, sp, h.done)
	}
}

func (s *Supervisor) shutdown() {
	// Mutually exclusive with Reconcile so a reconcile in flight cannot
	// start a stream after shutdown has begun tearing everything down.
	s.reconcileMu.Lock()
	defer s.reconcileMu.Unlock()

	s.mu.Lock()
	handles := make([]*streamHandle, 0, len(s.running))
	for id, h := range s.running {
		handles = append(handles, h)
		delete(s.running, id)
	}
	// Clearing parent makes a later Reconcile call a clean no-op instead of
	// leaving a stale running/status entry behind.
	s.parent = nil
	s.mu.Unlock()
	for _, h := range handles {
		h.cancel()
	}
	for _, h := range handles {
		<-h.done
	}
}

func (s *Supervisor) superviseOne(ctx context.Context, sp Spec, done chan struct{}) {
	defer close(done)
	attempt := 0
	for {
		if ctx.Err() != nil {
			return
		}
		proc, err := s.launcher.Start(sp)
		if err != nil {
			s.record(sp.ID, Failed, attempt)
			if !s.sleep(ctx, s.backoff.Next(attempt)) {
				return
			}
			attempt++
			continue
		}
		s.record(sp.ID, Running, attempt)

		exited := make(chan struct{})
		go func() { _ = proc.Wait(); close(exited) }()
		select {
		case <-ctx.Done():
			s.stopProcess(proc, exited)
			return
		case <-exited:
		}

		attempt++
		s.record(sp.ID, Restarting, attempt)
		if !s.sleep(ctx, s.backoff.Next(attempt-1)) {
			return
		}
	}
}

// stopProcess asks a child to terminate, escalating to kill if it does not
// exit within the shutdown grace period.
func (s *Supervisor) stopProcess(proc Process, exited chan struct{}) {
	_ = proc.Signal(syscall.SIGTERM)
	t := time.NewTimer(s.shutdownGrace)
	defer t.Stop()
	select {
	case <-exited:
	case <-t.C:
		_ = proc.Signal(syscall.SIGKILL)
		<-exited
	}
}

func (s *Supervisor) record(id string, st State, restarts int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	cur := s.status[id]
	if cur == nil {
		cur = &StreamStatus{ID: id}
		s.status[id] = cur
	}
	cur.State = st
	cur.Restarts = restarts
}

// sleep waits d or returns false if ctx is cancelled first.
func (s *Supervisor) sleep(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

// Status returns a snapshot of all streams.
func (s *Supervisor) Status() []StreamStatus {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]StreamStatus, 0, len(s.status))
	for _, st := range s.status {
		out = append(out, *st)
	}
	return out
}
