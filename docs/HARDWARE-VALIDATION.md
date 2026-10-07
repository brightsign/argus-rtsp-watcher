# Hardware Validation Checklist

The automated test suite (`make run-tests`, `make run-integration-tests`) covers
every behavior that does not require a real NPU-equipped BrightSign player and
real RTSP cameras. `test/smoke_single_test.go` and `test/smoke_multi_test.go`
are tagged `integration`, drive the real `argus-engine`/`watcher` binaries
end to end, and `t.Skip` cleanly on any non-aarch64 host (ruling P1) rather
than weakening their assertions — on a real player they run for real.

This document is the checklist for what those tests structurally cannot cover:
real cameras, real NPU contention across processes, and real memory pressure.
It is run by hand on each hardware tier before a release. Nothing here is
automatable in CI.

## 0. Before you start

- Build and package: `make build` (produces `staging/<SOC>/` and the
  `argus-rtsp-watcher-ext-*.zip` for the target you're validating).
- Make sure no stray `config.json` is sitting at any of the higher-priority
  discovery paths on the device (`/storage/sd/config.json`,
  `/storage/sd/configs/config.json`, `/storage/ssd/config.json`,
  `/storage/ssd/configs/config.json`) — config discovery is first-found-wins
  with no CLI/env override (see `docs/DESIGN.md` §5.3), so a leftover file
  from a previous validation pass silently wins over the one you think you're
  editing.
- Make sure no previous `watcher` process (and therefore no previous
  `mosquitto`) is still running — the watcher binds fixed ports (MQTT 1883,
  `/healthz` 9200) with no override, so a leaked prior instance will make the
  new one fail to bind rather than cleanly replace it.
- Have 3 RTSP cameras (or an RTSP re-streamer) reachable from the player's
  network for the XT5 pass; 2-3 for XS6/XD6; 1 for RK3568/LS5/HS5.

## 1. Install and configure (per device under test)

1. Insecure the player and install the extension
   (`argus-rtsp-watcher-ext-*.zip`) — see the `brightsign-native-extensions`
   and `brightsign-player-ssh` skills for the insecure/install/SSH mechanics
   specific to this BrightSign OS version.
2. Write `config.json` to `/storage/sd/configs/config.json` (or `/storage/sd/config.json`) with the tier's stream count (3 for
   XT5, 2-3 for XS6/XD6, 1 for RK3568), each entry pointing at a real
   `rtsp_url` — see `examples/config.example.json` for the shape.
3. Start (or restart) the extension and confirm in the log that it found
   your config: `using config /storage/sd/config.json` (not the packaged
   default).

## 2. Per-stream throughput

For each configured stream:

- [ ] Subscribe to `bs/argus/<id>/analytics` (e.g.
      `mosquitto_sub -h <player-ip> -t 'bs/argus/#' -v`) and confirm messages
      arrive at the configured `period_ms` cadence.
- [ ] Confirm the payload's `fps` field sustains 5-10 fps (the documented
      operating range; global-constraints.md, `target_fps` default 10).
- [ ] Confirm `health.detector_fps` and `health.tracker_fps` are consistent
      with `fps` and not collapsing toward 0 under the full configured stream
      count (evidence of NPU starvation — see §4).

## 3. Per-tier RAM headroom (DESIGN.md §12 risk)

Each engine process loads its own copy of the model weights (RetinaFace +
YOLOX), so footprint scales with stream count, not amortized.

- [ ] With the tier's full stream count running, record RSS per `argus-engine`
      process and the watcher process (`ps -o pid,rss,cmd -C argus-engine`,
      plus the watcher's own RSS) and total free memory (`free -m` or
      `/proc/meminfo`).
- [ ] Confirm headroom remains after the presentation/player's own normal
      workload is also running (not an idle box) — this is the number that
      decides whether a tier's documented safe capacity (DESIGN.md §9) needs
      to be lowered.
- [ ] If headroom is tight at the tier's documented capacity, note the actual
      measured RSS per stream here and flag it for a DESIGN.md §9 revision
      rather than silently shipping over budget.

## 4. Crash isolation and recovery (real camera)

- [ ] With all streams running and publishing, physically disconnect one
      camera (unplug it, or pull its network link).
- [ ] Confirm the other streams' topics keep publishing without interruption
      (crash isolation — matches `test/smoke_multi_test.go`'s file-input
      version of this same assertion, but here against a real RTSP source
      failure instead of a killed process).
- [ ] Confirm the disconnected stream's engine restarts (with backoff) rather
      than the whole watcher process exiting, and that `/healthz` reflects
      that stream's state (`restarting` / `failed`) without affecting the
      others' `running` state.
- [ ] Reconnect the camera and confirm that stream resumes publishing on its
      own, without restarting the watcher.

## 5. Repeat per tier

- [ ] **XT5 (RK3588):** 3 streams, all of §2-§4 above.
- [ ] **XS6/XD6 (RK3576):** 2-3 streams, all of §2-§4 above. DESIGN.md §9 marks
      this tier's capacity as "validate" — record whether 3 streams actually
      sustains 5-10 fps here or whether 2 is the real safe number.
- [ ] **RK3568 (LS5/HS5):** 1 stream, §2-§4 above (multi-stream is not
      supported at target fps on this tier; confirm the single stream still
      hits 5-10 fps and that `/healthz` does not report over-capacity).

## 6. Run the tagged integration tests on-device

On an aarch64 build host with the staged assets present (or directly on a
player with a Go toolchain), the same preconditions
`test/smoke_single_test.go` and `test/smoke_multi_test.go` check for are met,
so they run for real instead of skipping:

- [ ] `make run-integration-tests` — both `TestSingleStreamSmokeEndToEnd` and
      `TestMultiStreamIsolationAndReconcile` **PASS** (not SKIP). A SKIP here
      means one of the documented preconditions (staged binaries, test video,
      GOARCH) still isn't met on this host, not that the checklist item is
      satisfied.

## 7. Browser preview

Each stream's annotated frames (person boxes + gaze) are viewable live in a
browser as MJPEG, without any subscriber setup. This is on by default
(`enable_frame_output` is always `true` — see `internal/configgen.Render`),
so it should just work once the extension is running.

- The engine writes its annotated frame to
  `/tmp/argus-frames/<stream-id>/output.jpg` on the device, overwritten
  continuously.
- One `image-stream-server` process per stream (staged at
  `<SOC>/image-stream-server`, launched by the watcher alongside the engine)
  serves that file as MJPEG on its own port: `20200 + <stream's 0-based
  index in config.json>`.
- [ ] Confirm each configured stream is viewable at
      `http://<player-ip>:<port>` and shows the live person/gaze overlay
      boxes updating.
- [ ] Confirm the player's network allows inbound TCP on the full
      `20200..20200+N-1` range for an N-stream config (no separate opt-in —
      these ports come up whenever the corresponding stream does).

Example, for the sample 3-stream `config.json` below:

```json
{
  "streams": [
    { "id": "lobby",      "rtsp_url": "rtsp://192.0.2.10:8554/lobby" },
    { "id": "entrance", "rtsp_url": "rtsp://192.0.2.10:8554/entrance" },
    { "id": "aisle",   "rtsp_url": "rtsp://192.0.2.10:8554/aisle" }
  ]
}
```

| stream (index)      | preview URL                    | annotated frame on device                      |
|----------------------|---------------------------------|-------------------------------------------------|
| `lobby` (0)         | `http://<player-ip>:20200`      | `/tmp/argus-frames/lobby/output.jpg`           |
| `entrance` (1)    | `http://<player-ip>:20201`      | `/tmp/argus-frames/entrance/output.jpg`      |
| `aisle` (2)      | `http://<player-ip>:20202`      | `/tmp/argus-frames/aisle/output.jpg`        |

## 8. Deferred concerns worth checking on hardware

These were identified during implementation as plausible but unverified on
real hardware; each has a concrete signal to look for:

- [ ] **`engineEnviron` has no `PATH`/`HOME`** (`cmd/watcher/main.go`'s
      `engineEnviron` sets only `LD_LIBRARY_PATH`/`GST_*`). Confirm GStreamer
      still initializes cleanly for every engine — no `gst-plugin-scanner`
      failures or missing-`HOME` warnings in the engine logs.
- [ ] **NPU all-core scheduling** (`RKNN_NPU_CORE_0_1_2`, DESIGN.md §4.3.1):
      with the tier's full stream count running concurrently, confirm
      per-stream fps still meets the 5-10 target rather than degrading from
      NPU contention (§2's `fps`/`health.*_fps` fields are the signal).
- [ ] **Per-stream `GST_REGISTRY`** (DESIGN.md §4.3.2/§7.3): start the full
      stream count together (cold start, no pre-warmed registry) and confirm
      no engine fails or hangs during GStreamer plugin registry init — this
      is the race the per-instance `GST_REGISTRY=/tmp/gst-<id>.bin` path is
      meant to prevent.
- [ ] **`/healthz` over-capacity warning** (DESIGN.md §8): configure more
      streams than the tier's safe capacity and confirm both the startup log
      line and `/healthz`'s `over_capacity` field report it.
- [ ] **mosquitto readiness** (DESIGN.md §7.1 step 3): confirm the engine
      logs show no early MQTT connection-refused errors at startup — i.e.
      `broker.WaitForBroker` is actually gating engine spawn until mosquitto
      accepts connections, not racing it.
