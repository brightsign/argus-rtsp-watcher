package platform

import "testing"

func TestDetectSoC(t *testing.T) {
	cases := []struct {
		in   string
		want SoC
	}{
		{"rockchip,rk3588\x00brightsign,xt5", RK3588},
		{"brightsign,xs5\x00rockchip,rk3576", RK3576},
		{"rockchip,rk3568", RK3568},
		{"rockchip,rk3399", Unknown},
	}
	for _, c := range cases {
		if got := DetectSoC([]byte(c.in)); got != c.want {
			t.Errorf("DetectSoC(%q)=%v want %v", c.in, got, c.want)
		}
	}
}

func TestDetectSoCFromFileMissing(t *testing.T) {
	if got := DetectSoCFromFile("/nonexistent/path/compatible"); got != Unknown {
		t.Errorf("DetectSoCFromFile(nonexistent)=%v want Unknown", got)
	}
}

func TestTierCapacity(t *testing.T) {
	cases := []struct {
		soc  SoC
		want int
	}{
		{RK3588, 8},
		{RK3576, 4},
		{RK3568, 1},
		{Unknown, 1},
	}
	for _, c := range cases {
		if got := TierFor(c.soc).SafeStreams; got != c.want {
			t.Errorf("SafeStreams(%v)=%d want %d", c.soc, got, c.want)
		}
	}
}
