package supervisor

import (
	"os"
	"time"
)

// Liveness watchdog defaults. The timeout sits deliberately above the engine's
// own internal recovery watchdog so a healthy in-process recovery restarts the
// pipeline first; this external watchdog is the backstop for a process that is
// alive but wedged so hard its own recovery (and even its self-exit) never run.
const (
	defaultLivenessTimeout      = 30 * time.Second
	defaultLivenessStartupGrace = 90 * time.Second
	defaultLivenessInterval     = 5 * time.Second
)

// livenessState tracks output-frame progress for one running process so the
// supervisor can tell an alive-but-hung engine from a healthy one.
type livenessState struct {
	startedAt    time.Time
	lastMtime    time.Time // most recent output mtime observed
	lastProgress time.Time // wallclock when lastMtime last advanced
}

// stalled records the output file's mtime observed at now and reports whether
// the process has produced no new output for longer than timeout. During the
// first startupGrace after the process started it always returns false, so an
// engine still opening its RTSP stream is never killed prematurely. A zero
// mtime (a missing file) counts as no output.
func (st *livenessState) stalled(now, mtime time.Time, startupGrace, timeout time.Duration) bool {
	if mtime.After(st.lastMtime) {
		st.lastMtime = mtime
		st.lastProgress = now
	}
	if now.Sub(st.startedAt) < startupGrace {
		return false
	}
	ref := st.lastProgress
	if ref.IsZero() {
		ref = st.startedAt
	}
	return now.Sub(ref) > timeout
}

// statFileMtime returns the file's modification time, or ok=false when it
// cannot be stat'd (for example it does not exist yet).
func statFileMtime(path string) (time.Time, bool) {
	fi, err := os.Stat(path)
	if err != nil {
		return time.Time{}, false
	}
	return fi.ModTime(), true
}
