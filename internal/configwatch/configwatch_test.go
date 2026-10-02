package configwatch

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestWatchFiresOnChange(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "watcher.json")
	if err := os.WriteFile(p, []byte(`{"v":1}`), 0o644); err != nil {
		t.Fatal(err)
	}
	fired := make(chan struct{}, 1)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go Watch(ctx, p, 10*time.Millisecond, func() { fired <- struct{}{} })

	// mtime resolution can be coarse; make the change unambiguous in time.
	time.Sleep(20 * time.Millisecond)
	future := time.Now().Add(2 * time.Second)
	os.Chtimes(p, future, future)

	select {
	case <-fired:
	case <-time.After(2 * time.Second):
		t.Fatal("onChange was not called after file changed")
	}
}
