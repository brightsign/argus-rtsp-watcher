package health

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"argus-rtsp-watcher/internal/platform"
	"argus-rtsp-watcher/internal/supervisor"
)

func TestHealthReportsTierAndStreams(t *testing.T) {
	tier := platform.Tier{SoC: platform.RK3588, Model: "XT5", SafeStreams: 3}
	h := Handler(tier, 2, func() []supervisor.StreamStatus {
		return []supervisor.StreamStatus{{ID: "a", State: supervisor.Running, Restarts: 0}}
	})
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("code=%d", rec.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body["soc"] != "RK3588" {
		t.Errorf("soc=%v", body["soc"])
	}
	if body["over_capacity"] != false {
		t.Errorf("over_capacity=%v", body["over_capacity"])
	}
}

func TestHealthFlagsOverCapacity(t *testing.T) {
	tier := platform.Tier{SoC: platform.RK3568, Model: "LS5", SafeStreams: 1}
	h := Handler(tier, 3, func() []supervisor.StreamStatus { return nil })
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	var body map[string]any
	json.Unmarshal(rec.Body.Bytes(), &body)
	if body["over_capacity"] != true {
		t.Errorf("expected over_capacity true")
	}
}
