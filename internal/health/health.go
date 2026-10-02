// Package health serves the watcher's /healthz status.
package health

import (
	"encoding/json"
	"net/http"

	"argus-rtsp-watcher/internal/platform"
	"argus-rtsp-watcher/internal/supervisor"
)

// StatusFunc returns the current per-stream status snapshot.
type StatusFunc func() []supervisor.StreamStatus

// Handler serves a JSON health document at any path.
func Handler(tier platform.Tier, configured int, status StatusFunc) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		doc := map[string]any{
			"soc":           string(tier.SoC),
			"model":         tier.Model,
			"safe_streams":  tier.SafeStreams,
			"configured":    configured,
			"over_capacity": configured > tier.SafeStreams,
			"streams":       status(),
		}
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(doc); err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
		}
	})
}
