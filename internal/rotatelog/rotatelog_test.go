package rotatelog

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func readFile(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	return string(data)
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func mustWrite(t *testing.T, w *Writer, text string) {
	t.Helper()
	// Errorf, not Fatalf: this is also called from goroutines, where FailNow is not allowed.
	n, err := w.Write([]byte(text))
	if err != nil || n != len(text) {
		t.Errorf("Write(%q) = %d, %v", text, n, err)
	}
}

func TestWritesAppendToTheFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	w, err := New(path, 1000, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	mustWrite(t, w, "one\n")
	mustWrite(t, w, "two\n")

	if got := readFile(t, path); got != "one\ntwo\n" {
		t.Errorf("file = %q, want both lines", got)
	}
}

func TestCreatesAMissingDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "logs", "nested", "main.log")
	w, err := New(path, 1000, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	mustWrite(t, w, "hello\n")
	if got := readFile(t, path); got != "hello\n" {
		t.Errorf("file = %q", got)
	}
}

func TestReopeningContinuesTheSizeOfAnExistingFile(t *testing.T) {
	// A restart must not forget how full the file already is, or it would grow
	// to nearly twice the limit before its first rotation.
	path := filepath.Join(t.TempDir(), "main.log")
	first, err := New(path, 100, 3)
	if err != nil {
		t.Fatal(err)
	}
	mustWrite(t, first, strings.Repeat("a", 60))
	first.Close()

	second, err := New(path, 100, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	mustWrite(t, second, strings.Repeat("b", 60))

	if got := readFile(t, path+".1"); got != strings.Repeat("a", 60) {
		t.Errorf("rotated file = %q, want the 60 bytes written before the restart", got)
	}
	if got := readFile(t, path); got != strings.Repeat("b", 60) {
		t.Errorf("current file = %q", got)
	}
}

func TestRotatesBeforeAWriteThatWouldExceedTheLimit(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	w, err := New(path, 100, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	chunk := strings.Repeat("x", 40)
	mustWrite(t, w, chunk)
	mustWrite(t, w, chunk) // 80 bytes: fits
	mustWrite(t, w, chunk) // would make 120: rotate first

	if got := readFile(t, path+".1"); got != chunk+chunk {
		t.Errorf(".1 = %d bytes, want the first two chunks (80)", len(got))
	}
	if got := readFile(t, path); got != chunk {
		t.Errorf("current = %d bytes, want the third chunk (40)", len(got))
	}
}

func TestAWriteExactlyFillingTheLimitDoesNotRotate(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	w, err := New(path, 100, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	mustWrite(t, w, strings.Repeat("x", 100))
	if exists(path + ".1") {
		t.Error("rotated at exactly the limit; the limit is inclusive")
	}
}

func TestKeepsOnlyMaxFilesAndDropsTheOldest(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	w, err := New(path, 10, 3)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	// Each 8-byte line forces a rotation on the next one: 6 lines, 5 rotations.
	for i := 1; i <= 6; i++ {
		mustWrite(t, w, fmt.Sprintf("line-%03d\n", i)[:8])
	}

	for _, kept := range []string{"", ".1", ".2", ".3"} {
		if !exists(path + kept) {
			t.Errorf("expected %s%s to exist", filepath.Base(path), kept)
		}
	}
	if exists(path + ".4") {
		t.Error("kept more than maxFiles rotated files")
	}
	// Newest in the base file, oldest retained in .3 (lines 1 and 2 are gone).
	if got := readFile(t, path); got != "line-006" {
		t.Errorf("current = %q, want line-006", got)
	}
	if got := readFile(t, path+".3"); got != "line-003" {
		t.Errorf(".3 = %q, want line-003 (the oldest still kept)", got)
	}
}

func TestASingleWriteLargerThanTheLimitGoesIntoAFreshFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	w, err := New(path, 10, 2)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	mustWrite(t, w, "small\n")
	huge := strings.Repeat("h", 50)
	mustWrite(t, w, huge)

	if got := readFile(t, path); got != huge {
		t.Errorf("current = %d bytes, want the 50-byte write whole", len(got))
	}
	if got := readFile(t, path+".1"); got != "small\n" {
		t.Errorf(".1 = %q", got)
	}
	// And it must not loop forever rotating: the next small write lands after it.
	mustWrite(t, w, "after\n")
}

func TestConcurrentWritersLoseNoLineAndNeverSplitOne(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.log")
	const writers, linesEach, keep = 8, 200, 1000
	w, err := New(path, 512, keep)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()

	var wg sync.WaitGroup
	for id := 0; id < writers; id++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for i := 0; i < linesEach; i++ {
				mustWrite(t, w, fmt.Sprintf("writer-%d line-%04d padding-padding\n", id, i))
			}
		}(id)
	}
	wg.Wait()

	seen := map[string]int{}
	files, err := filepath.Glob(path + "*")
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		for _, line := range strings.Split(strings.TrimSuffix(readFile(t, f), "\n"), "\n") {
			var id, i int
			if _, err := fmt.Sscanf(line, "writer-%d line-%04d padding-padding", &id, &i); err != nil {
				t.Fatalf("a line was split or corrupted: %q (%v)", line, err)
			}
			seen[line]++
		}
	}
	if len(seen) != writers*linesEach {
		t.Errorf("found %d distinct lines, want %d: lines were lost", len(seen), writers*linesEach)
	}
	for line, count := range seen {
		if count != 1 {
			t.Errorf("line %q appears %d times", line, count)
		}
	}
}

func TestRejectsInvalidLimits(t *testing.T) {
	dir := t.TempDir()
	if _, err := New(filepath.Join(dir, "a.log"), 0, 3); err == nil {
		t.Error("maxBytes 0 accepted")
	}
	if _, err := New(filepath.Join(dir, "b.log"), 100, 0); err == nil {
		t.Error("maxFiles 0 accepted")
	}
}
