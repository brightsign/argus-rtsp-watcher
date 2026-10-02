// Package platform detects the Rockchip SoC and its safe stream capacity.
package platform

import (
	"os"
	"strings"
)

// SoC identifies the Rockchip system-on-chip family.
type SoC string

const (
	RK3588  SoC = "RK3588"
	RK3576  SoC = "RK3576"
	RK3568  SoC = "RK3568"
	Unknown SoC = "UNKNOWN"
)

const (
	multiStreamCapacity  = 3
	singleStreamCapacity = 1
)

// Tier describes a SoC's audience-measurement stream capacity.
type Tier struct {
	SoC         SoC
	Model       string
	SafeStreams int
}

// deviceTreeCompatible is the path whose bytes name the SoC on a player.
const deviceTreeCompatible = "/sys/firmware/devicetree/base/compatible"

// DetectSoC parses a devicetree "compatible" blob (NUL-separated strings).
func DetectSoC(compatible []byte) SoC {
	s := string(compatible)
	switch {
	case strings.Contains(s, "rockchip,rk3588"):
		return RK3588
	case strings.Contains(s, "rockchip,rk3576"):
		return RK3576
	case strings.Contains(s, "rockchip,rk3568"):
		return RK3568
	default:
		return Unknown
	}
}

// DetectSoCFromFile reads the devicetree compatible file; Unknown on error.
func DetectSoCFromFile(path string) SoC {
	data, err := os.ReadFile(path)
	if err != nil {
		return Unknown
	}
	return DetectSoC(data)
}

// DetectPlayerSoC reads the real devicetree path.
func DetectPlayerSoC() SoC { return DetectSoCFromFile(deviceTreeCompatible) }

// TierFor returns the capacity tier for a SoC. Unknown is treated
// conservatively as single-stream.
func TierFor(soc SoC) Tier {
	switch soc {
	case RK3588:
		return Tier{SoC: RK3588, Model: "XT5", SafeStreams: multiStreamCapacity}
	case RK3576:
		return Tier{SoC: RK3576, Model: "XS156", SafeStreams: multiStreamCapacity}
	case RK3568:
		return Tier{SoC: RK3568, Model: "LS5/HS5", SafeStreams: singleStreamCapacity}
	default:
		return Tier{SoC: Unknown, Model: "unknown", SafeStreams: singleStreamCapacity}
	}
}
