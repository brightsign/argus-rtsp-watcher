// Package broker starts the local mosquitto broker and detects readiness.
package broker

import (
	"fmt"
	"net"
	"os/exec"
	"syscall"
	"time"
)

// Start launches mosquitto with the given config path, in its own process
// group, so it can be stopped independently of the watcher's own group.
func Start(mosquittoBin, confPath string) (*exec.Cmd, error) {
	cmd := exec.Command(mosquittoBin, "-c", confPath)
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start mosquitto: %w", err)
	}
	return cmd, nil
}

// Stop signals the mosquitto process group to terminate and waits for it
// to exit, so a later restart never binds a stale broker on the same port.
// An exit triggered by the signal itself is not treated as a failure.
func Stop(cmd *exec.Cmd) error {
	if cmd == nil || cmd.Process == nil {
		return nil
	}
	if err := syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM); err != nil {
		return fmt.Errorf("signal mosquitto: %w", err)
	}
	if err := cmd.Wait(); err != nil {
		if _, ok := err.(*exec.ExitError); ok {
			return nil
		}
		return fmt.Errorf("wait mosquitto: %w", err)
	}
	return nil
}

// WaitForBroker returns nil once addr accepts a TCP connection, or an
// error if timeout elapses first.
func WaitForBroker(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
		if err == nil {
			conn.Close()
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return fmt.Errorf("broker at %s not ready within %s", addr, timeout)
}
