// Package rotatelog is a size-rotating log file writer. Files are named path,
// path.1, path.2, ... newest first, the same scheme the engine's own logger
// uses, so every log file on the player rotates the same way.
package rotatelog

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

const (
	directoryMode = 0o755
	fileMode      = 0o644
)

// Writer appends to a log file and rotates it once it would exceed a size.
// It is safe for concurrent use; each Write lands whole in one file, so a
// caller that writes whole lines never has a line split across a rotation.
type Writer struct {
	mu       sync.Mutex
	path     string
	maxBytes int64
	maxFiles int
	file     *os.File
	size     int64
}

// New opens (creating it and its directory if needed) the log file at path.
// A file larger than maxBytes is rotated before the next write; maxFiles
// rotated files (path.1 .. path.maxFiles) are kept and older ones deleted.
func New(path string, maxBytes int64, maxFiles int) (*Writer, error) {
	if maxBytes <= 0 {
		return nil, fmt.Errorf("rotatelog: maxBytes must be positive, got %d", maxBytes)
	}
	if maxFiles < 1 {
		return nil, fmt.Errorf("rotatelog: maxFiles must be at least 1, got %d", maxFiles)
	}
	if err := os.MkdirAll(filepath.Dir(path), directoryMode); err != nil {
		return nil, fmt.Errorf("rotatelog: create directory: %w", err)
	}
	w := &Writer{path: path, maxBytes: maxBytes, maxFiles: maxFiles}
	if err := w.open(); err != nil {
		return nil, err
	}
	return w, nil
}

// Write appends p, rotating first when p would push the file past its limit.
// A single write larger than the limit goes into a fresh file on its own.
func (w *Writer) Write(p []byte) (int, error) {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.size > 0 && w.size+int64(len(p)) > w.maxBytes {
		if err := w.rotate(); err != nil {
			return 0, err
		}
	}
	n, err := w.file.Write(p)
	w.size += int64(n)
	return n, err
}

// Close closes the current file.
func (w *Writer) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.file.Close()
}

func (w *Writer) open() error {
	file, err := os.OpenFile(w.path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, fileMode)
	if err != nil {
		return fmt.Errorf("rotatelog: open %s: %w", w.path, err)
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return fmt.Errorf("rotatelog: stat %s: %w", w.path, err)
	}
	w.file = file
	w.size = info.Size()
	return nil
}

// rotate shifts path.N-1 to path.N ... path to path.1 and reopens path empty.
// The oldest file falls off the end.
func (w *Writer) rotate() error {
	if err := w.file.Close(); err != nil {
		return fmt.Errorf("rotatelog: close %s: %w", w.path, err)
	}
	if err := os.Remove(w.numbered(w.maxFiles)); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("rotatelog: remove oldest: %w", err)
	}
	for i := w.maxFiles - 1; i >= 1; i-- {
		if err := os.Rename(w.numbered(i), w.numbered(i+1)); err != nil && !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("rotatelog: shift %s: %w", w.numbered(i), err)
		}
	}
	if err := os.Rename(w.path, w.numbered(1)); err != nil {
		return fmt.Errorf("rotatelog: rotate %s: %w", w.path, err)
	}
	return w.open()
}

func (w *Writer) numbered(n int) string {
	return fmt.Sprintf("%s.%d", w.path, n)
}
