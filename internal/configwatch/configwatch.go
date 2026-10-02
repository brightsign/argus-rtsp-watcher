// Package configwatch triggers a callback when a watched file changes,
// using stdlib mtime polling (no external dependencies).
package configwatch

import (
	"context"
	"os"
	"time"
)

// ModTime returns the modification time of path.
func ModTime(path string) (time.Time, error) {
	info, err := os.Stat(path)
	if err != nil {
		return time.Time{}, err
	}
	return info.ModTime(), nil
}

// Watch polls path every interval and calls onChange whenever its mtime
// differs from the last observed value. It returns when ctx is cancelled.
func Watch(ctx context.Context, path string, interval time.Duration, onChange func()) {
	last, _ := ModTime(path)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			cur, err := ModTime(path)
			if err != nil {
				continue
			}
			if !cur.Equal(last) {
				last = cur
				onChange()
			}
		}
	}
}
