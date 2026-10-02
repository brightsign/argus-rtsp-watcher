package supervisor

import (
	"testing"
	"time"
)

func TestBackoffGrowsAndCaps(t *testing.T) {
	b := Backoff{Min: 250 * time.Millisecond, Max: 8 * time.Second}
	if b.Next(0) != 250*time.Millisecond {
		t.Errorf("attempt 0 = %v", b.Next(0))
	}
	if b.Next(1) != 500*time.Millisecond {
		t.Errorf("attempt 1 = %v", b.Next(1))
	}
	if b.Next(2) != time.Second {
		t.Errorf("attempt 2 = %v", b.Next(2))
	}
	if b.Next(100) != 8*time.Second {
		t.Errorf("attempt 100 should cap at Max, got %v", b.Next(100))
	}
}
