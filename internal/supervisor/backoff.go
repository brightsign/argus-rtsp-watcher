package supervisor

import "time"

// Backoff computes bounded exponential restart delays.
type Backoff struct {
	Min time.Duration
	Max time.Duration
}

// Next returns the delay for a zero-based restart attempt, doubling Min
// each attempt and capping at Max.
func (b Backoff) Next(attempt int) time.Duration {
	d := b.Min
	for i := 0; i < attempt; i++ {
		d *= 2
		if d >= b.Max {
			return b.Max
		}
	}
	if d > b.Max {
		return b.Max
	}
	return d
}
