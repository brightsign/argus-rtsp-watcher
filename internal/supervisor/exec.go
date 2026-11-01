package supervisor

import (
	"os"
	"os/exec"
	"syscall"
	"time"
)

// ExecLauncher starts engine processes as real OS processes, each in its
// own process group.
type ExecLauncher struct {
	WorkDir string
}

type execProcess struct {
	cmd *exec.Cmd
}

func (p *execProcess) Wait() error { return p.cmd.Wait() }

// Signal sends sig to the child's whole process group.
func (p *execProcess) Signal(sig os.Signal) error {
	s, ok := sig.(syscall.Signal)
	if !ok {
		return p.cmd.Process.Signal(sig)
	}
	return syscall.Kill(-p.cmd.Process.Pid, s)
}

// outputDrainDelay bounds how long Wait lingers for a child's output to drain
// after the child itself has exited. A grandchild that inherited the output pipe
// (MediaMTX can run helper commands) would otherwise keep Wait blocked forever.
const outputDrainDelay = 2 * time.Second

// Start launches the process: s.Bin with s.Args.
func (l ExecLauncher) Start(s Spec) (Process, error) {
	cmd := exec.Command(s.Bin, s.Args...)
	cmd.Dir = l.WorkDir
	cmd.Env = s.Env
	if s.Output != nil {
		cmd.Stdout = s.Output
		cmd.Stderr = s.Output
		cmd.WaitDelay = outputDrainDelay
	} else {
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
	}
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return &execProcess{cmd: cmd}, nil
}
