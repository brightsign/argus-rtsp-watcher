package supervisor

import (
	"os"
	"os/exec"
	"syscall"
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

// Start launches the process: s.Bin with s.Args.
func (l ExecLauncher) Start(s Spec) (Process, error) {
	cmd := exec.Command(s.Bin, s.Args...)
	cmd.Dir = l.WorkDir
	cmd.Env = s.Env
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return &execProcess{cmd: cmd}, nil
}
