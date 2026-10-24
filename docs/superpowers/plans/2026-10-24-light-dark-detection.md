# Light/Dark Scene Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect whether the real-world scene in each camera view is light or dark (robust to IR/night mode) and publish it over MQTT as a new retained topic plus additive fields on the existing analytics message.

**Architecture:** A pure `LightAnalyzer` runs in the engine capture thread, measuring mean luminance and mean chroma over a central ROI of each `SharedFrame.bgr`, smoothing with an EMA, and running a hysteresis + dwell state machine whose primary darkness signal is IR/monochrome mode (chroma near zero). The orchestrator carries the resulting `LightState` into `PipelineResult`; the MQTT publisher emits it as scene-level and per-track fields on `.../analytics` and on a new retained `.../light` topic (periodic + on-change). Per-stream config flows through the existing Go `configgen` -> engine `configuration.cpp` -> `PipelineConfig`/`MqttPublisherCfg` path, mirroring how `zone` is threaded today.

**Tech Stack:** C++17 (GoogleTest v1.14 host tests), nlohmann/json, libmosquitto; Go (stdlib + existing internal packages); builds and tests only through the Makefile.

**Spec:** `docs/superpowers/specs/2026-10-24-light-dark-detection-design.md`

## Global Constraints

- Build and test ONLY through the Makefile: `make build`, `make run-tests` (Go + engine unit tests), `make run-engine-tests`, `make build-engine-tests`. Never invoke `go`/`cmake`/`ctest` directly.
- Analytics schema string bumps `analytics/v7.0` -> `analytics/v7.1`; all additions are additive and backward compatible. When a stream's light detection is disabled, no `light` scene object or per-track field is emitted and the `.../light` topic is silent.
- The `.../light` topic is published with `retain: true` and `qos: 1` (deliberate deviation from the non-retained `zone_event`); the `.../analytics` topic keeps its configured qos/retain.
- The `.../light` topic is derived from the analytics topic exactly as `zone_event` is: `.../analytics` -> `.../light`, else `<topic>/light`.
- Per-stream defaults (the single source of truth) from spec §4.5: `enabled=true`, `interval_seconds=10`, `dark_luma=40`, `light_luma=70`, `chroma_threshold=10`, `ema_alpha=0.1`, `min_dwell_seconds=5`, `warmup_seconds=3`, `roi_frac=0.6`, `sample_stride=8`, `monochrome_camera=false`, `on_change=true`. Go `configgen` renders a fully-populated `light` object (applies all defaults); C++ keeps the same defaults only as a fallback. Keep the two tables in sync (comment each pointing at the spec).
- `SharedFrame.bgr` is the full-resolution, unpadded camera frame (e.g. 1920x1080 BGR24), not a 320x320 letterboxed buffer. The analyzer iterates `SharedFrame.width x SharedFrame.height`.
- Luma = BT.601: `0.299*R + 0.587*G + 0.114*B`. Chroma = `max(R,G,B) - min(R,G,B)`. `level` 0..100 = 0 in IR mode, else `round(100*ema_luma/255)` clamped.
- C++ code quality / security: 64-bit accumulators for pixel sums (CWE-190), `snprintf` with fixed format strings and `%s` for string values (CWE-134), RAII `std::lock_guard` for all shared state (no manual lock/unlock), bounds-checked pixel indexing, initialize all struct members. No emoji in code. Comments explain WHY not WHAT; no commented-out code.
- No new or changed network API surface (HTTP/gRPC): api-canary is N/A for this feature; the only new transport is the local MQTT broker pub/sub. Do not add canary tasks.

## Review Focus

- **Tiny or empty frame** (`bgr.size()==0`, or `width`/`height` smaller than the derived stride): analyzer must not divide by zero or index out of bounds; it returns `UNKNOWN` with zeroed metrics and never crashes. Pinned in Task 1.
- **Pixel-sum overflow**: at 1080p a naive 32-bit accumulator over ~2M pixels x 255 overflows; sums must use 64-bit accumulators. Pinned in Task 1.
- **Invalid config values** (`dark_luma > light_luma`, `ema_alpha<=0 or >1`, `interval_seconds<1`, `sample_stride<1`, `roi_frac<=0 or >1`): Go validation rejects with a clear error; the C++ analyzer defensively clamps `sample_stride>=1` and `roi_frac` into `(0,1]` so a hand-written engine config cannot cause a bad access. Pinned in Tasks 6 (Go) and 1 (C++ clamp).
- **Monochrome-camera mode**: with `monochrome_camera=true` the chroma rule is skipped and a bright monochrome daylight image reads `LIGHT` (not `DARK`); with it `false`, the same frame reads `DARK` (IR). Pinned in Task 1.
- **State flapping at threshold**: luma oscillating inside the `[dark_luma, light_luma]` dead-band, and rapid candidate flips within `min_dwell_seconds`, must not change the committed state. Pinned in Task 1.

---

### Task 1: LightAnalyzer core (measurement + state machine)

**Files:**
- Create: `engine/include/analysis/light_config.h`
- Create: `engine/include/analysis/light_state.h`
- Create: `engine/include/analysis/light_analyzer.h`
- Create: `engine/src/analysis/light_analyzer.cpp`
- Test: `engine/tests/test_light_analyzer.cpp`
- Modify: `engine/tests/CMakeLists.txt` (add `../src/analysis/light_analyzer.cpp` to `TEST_DEPENDENCIES`)
- Modify: `engine/CMakeLists.txt` (add `src/analysis/light_analyzer.cpp` to the `argus-engine` source list)

**Interfaces:**
- Consumes: `SharedFrame` from `engine/include/pipeline/shared_frame.h` (`width`, `height`, `std::vector<uint8_t> bgr`).
- Produces:
  - `enum class LightLevelState : uint8_t { Unknown=0, Light=1, Dark=2 };` (`light_state.h`)
  - `struct LightState { LightLevelState state{LightLevelState::Unknown}; int level{0}; bool ir_mode{false}; float luma{0.f}; float chroma{0.f}; int64_t last_change_ms{0}; };` (`light_state.h`)
  - `struct LightCfg { bool enabled{true}; int interval_seconds{10}; bool on_change{true}; float dark_luma{40.f}; float light_luma{70.f}; float chroma_threshold{10.f}; float ema_alpha{0.1f}; int min_dwell_seconds{5}; int warmup_seconds{3}; float roi_frac{0.6f}; int sample_stride{8}; bool monochrome_camera{false}; };` (`light_config.h`)
  - `class LightAnalyzer { public: explicit LightAnalyzer(LightCfg cfg) noexcept; void update(const SharedFrame& sf, int64_t now_ms) noexcept; LightState snapshot() const noexcept; };`

- [ ] **Step 1: Write failing tests** in `engine/tests/test_light_analyzer.cpp` (GoogleTest, pattern per `engine/tests/test_zone.cpp`). Build synthetic BGR buffers with a helper `make_bgr(w,h,B,G,R)`. Tests (feed `update` enough frames to pass warmup, advancing `now_ms`; assert on `snapshot()`):
  - `BrightColorIsLight`: solid mid-grey-plus-colour (e.g. B=60,G=160,R=200 so luma>light_luma, chroma>chroma_threshold) -> `state==Light`, `ir_mode==false`, `level==round(100*luma/255)`.
  - `DarkColorIsDark`: dim colour (luma<dark_luma, chroma>chroma_threshold) -> `state==Dark`, `ir_mode==false`.
  - `BrightMonochromeIsDarkViaIR`: bright but achromatic (B==G==R==200, chroma 0) with `monochrome_camera=false` -> `state==Dark`, `ir_mode==true`, `level==0`.
  - `BrightMonochromeIsLightWhenMonoCamera`: same bright achromatic frame with `monochrome_camera=true` -> `state==Light`, `ir_mode==false`.
  - `DeadBandHoldsState`: drive to `Light`, then feed frames with luma strictly between `dark_luma` and `light_luma` -> state stays `Light`.
  - `DwellSuppressesRapidFlip`: drive to `Light`; feed one dark frame but advance `now_ms` by less than `min_dwell_seconds*1000` -> state stays `Light`; after dwell elapses with dark frames -> `Dark`.
  - `WarmupIsUnknown`: fewer than `warmup_seconds` worth of samples -> `state==Unknown`.
  - `EmptyFrameIsSafe`: `SharedFrame` with empty `bgr` -> no crash, `state==Unknown`.
  - `TinyFrameIsSafe`: 2x2 frame with `sample_stride=8` -> no crash, at least one pixel sampled, sane `luma`.
  - `FirstCommitRecordsChangeTimestamp`: `last_change_ms` is set to the `now_ms` of the first commit out of `Unknown`.

- [ ] **Step 2: Run tests, verify they fail.** Run: `make build-engine-tests` then `make run-engine-tests`. Expected: FAIL (LightAnalyzer undefined / link error).

- [ ] **Step 3: Implement the three headers** with the signatures in Interfaces. Defaults in `LightCfg` are the §4.5 values; add a comment pointing to the spec as source of truth.

- [ ] **Step 4: Implement `LightAnalyzer` in `engine/src/analysis/light_analyzer.cpp`.** Approach:
  - Constructor clamps defensively: `sample_stride = max(1, cfg.sample_stride)`, `roi_frac = clamp(cfg.roi_frac, epsilon, 1.0f)`.
  - `update()`: if `bgr.empty()` or `width<=0 || height<=0`, return (state unchanged; if never warmed, stays `Unknown`). Compute the central ROI rectangle from `roi_frac`. Derive a resolution-relative stride: `stride = max(cfg.sample_stride, round(sqrt(roi_pixels / TARGET_SAMPLES)))` with `TARGET_SAMPLES` a named constant (~2048). Iterate the ROI with that stride; accumulate `sum_luma`, `sum_chroma`, `count` in `uint64_t` (chroma scaled x1000 or kept as rounded int to stay integer; luma likewise) to avoid float error and overflow. Guarantee `count>=1` for a non-empty frame. Convert to float means. Update EMAs (`ema += alpha*(sample-ema)`; seed on first sample). Run the state machine from spec §4.3 (warmup -> Unknown; chroma rule unless `monochrome_camera`; hysteresis dead-band; `min_dwell_seconds` gate; set `ir_mode`, `level`, `last_change_ms` on commit). Guard all shared reads/writes behind a `std::mutex` (RAII) since `update` runs on the capture thread and `snapshot` is read elsewhere.
  - `snapshot()`: return the current `LightState` under the lock.

- [ ] **Step 5: Register sources.** Add `../src/analysis/light_analyzer.cpp` to `TEST_DEPENDENCIES` in `engine/tests/CMakeLists.txt` and `src/analysis/light_analyzer.cpp` to the `argus-engine` target in `engine/CMakeLists.txt`.

- [ ] **Step 6: Run tests, verify they pass.** Run: `make run-engine-tests`. Expected: PASS (all `LightAnalyzer*` tests green).

- [ ] **Step 7: Commit.**
```bash
git add engine/include/analysis engine/src/analysis/light_analyzer.cpp engine/tests/test_light_analyzer.cpp engine/tests/CMakeLists.txt engine/CMakeLists.txt
git commit -m "feat(engine): LightAnalyzer for IR-aware light/dark scene detection"
```

---

### Task 2: Light serialization helpers (pure)

**Files:**
- Create: `engine/include/analysis/light_json.h`
- Create: `engine/src/analysis/light_json.cpp`
- Test: `engine/tests/test_light_json.cpp`
- Modify: `engine/tests/CMakeLists.txt`, `engine/CMakeLists.txt` (add `light_json.cpp`)

**Interfaces:**
- Consumes: `LightState`, `LightLevelState` (Task 1).
- Produces:
  - `const char* light_state_str(LightLevelState s) noexcept;` -> `"LIGHT"`, `"DARK"`, `"UNKNOWN"`.
  - `std::string light_scene_json(const LightState& s);` -> `{"state":"DARK","level":0,"ir_mode":true,"luma":182.4,"chroma":2.1}` (luma/chroma 1 decimal).
  - `std::string light_topic_json(const std::string& device_id, const LightState& s, const char* event, int64_t ts_ms);` -> full `.../light` payload (device_id via `%s`), `event` is `"periodic"` or `"change"`.
  - `bool light_changed(const LightState& a, const LightState& b) noexcept;` -> `a.state!=b.state || a.ir_mode!=b.ir_mode`.

- [ ] **Step 1: Write failing tests** in `engine/tests/test_light_json.cpp`:
  - `StateStrings`: each enum maps to its exact string.
  - `SceneJsonShape`: a known `LightState` serializes to the exact expected JSON string.
  - `TopicJsonShape`: includes `device_id`, `event`, `state`, `level`, `ir_mode`, `luma`, `chroma`, `ts_ms`.
  - `ChangedOnStateFlip` / `ChangedOnIrFlipSameState` (DARK+color -> DARK+ir returns true) / `NotChangedWhenIdentical`.
- [ ] **Step 2: Run, verify fail.** `make build-engine-tests && make run-engine-tests` -> FAIL.
- [ ] **Step 3: Implement** `light_json.cpp` using `snprintf` into a local buffer with fixed format strings (`%s` for `device_id`/state). Register sources in both CMake files.
- [ ] **Step 4: Run, verify pass.** `make run-engine-tests` -> PASS.
- [ ] **Step 5: Commit.** `feat(engine): light/dark MQTT serialization helpers`

---

### Task 3: Engine config parsing (`light` object)

**Files:**
- Modify: `engine/include/config/configuration.h` (add `struct Light { ... } light;` to `AppConfig`, fields mirroring `LightCfg`)
- Modify: `engine/src/config/configuration.cpp` (parse optional `"light"` object, same `j.contains(...)` pattern as the `zone` block ~line 137)
- Test: `engine/tests/test_config.cpp` (extend)

**Interfaces:**
- Produces: `AppConfig::Light` with the §4.5 fields and defaults; populated from JSON when a `"light"` object is present, otherwise left at defaults.

- [ ] **Step 1: Write failing tests** in `engine/tests/test_config.cpp` (reuse its existing temp-file + `config::load_from_file` helper):
  - `LightDefaultsWhenAbsent`: config without `light` -> `light.enabled==true`, `interval_seconds==10`, `dark_luma==40`, etc.
  - `LightParsedWhenPresent`: config with a `light` object overriding `enabled=false`, `interval_seconds=30`, `dark_luma=25`, `monochrome_camera=true` -> those values land in `AppConfig::light`.
- [ ] **Step 2: Run, verify fail.** `make build-engine-tests && make run-engine-tests` -> FAIL.
- [ ] **Step 3: Add `struct Light` to `AppConfig`** (`configuration.h`) with the §4.5 defaults and a comment pointing at the spec. **Parse** in `configuration.cpp`: read each sub-field defensively with the existing `is_number`/`is_boolean`/`is_string` guards.
- [ ] **Step 4: Run, verify pass.** `make run-engine-tests` -> PASS.
- [ ] **Step 5: Commit.** `feat(engine): parse per-stream light config`

---

### Task 4: Orchestrator wiring (per-frame analysis + carry in PipelineResult)

**Files:**
- Modify: `engine/include/pipeline/pipeline_types.h` (add `#include "analysis/light_state.h"`; add `LightState light{}; bool light_enabled{false};` to `PipelineResult`)
- Modify: `engine/include/orchestration/orchestrator.h` (add `#include "analysis/light_analyzer.h"`; add `LightCfg light;` to `PipelineConfig`; add members `std::unique_ptr<LightAnalyzer> light_analyzer_;`)
- Modify: `engine/src/orchestration/orchestrator.cpp` (construct analyzer when `cfg_.light.enabled`; call `light_analyzer_->update(*sf, wall_ms)` in `capture_loop_threadfn` after `sf->bgr` is filled; at the 1 Hz publish build, set `result.light_enabled = cfg_.light.enabled` and `result.light = light_analyzer_ ? light_analyzer_->snapshot() : LightState{}`)
- Modify: `engine/src/main.cpp` (map `appcfg.light` -> `pc.light` field-by-field, mirroring the `pc.zone` block ~line 317)

**Interfaces:**
- Consumes: `LightAnalyzer`, `LightCfg`, `LightState` (Tasks 1), `SharedFrame`.
- Produces: `PipelineResult.light` / `PipelineResult.light_enabled` for the publisher (Task 5); `PipelineConfig.light`.

- [ ] **Step 1: Map config** in `main.cpp`: copy every `appcfg.light.*` into `pc.light.*`.
- [ ] **Step 2: Wire the analyzer** in `orchestrator.cpp`: construct in the pipeline build when enabled; `update()` per captured frame using a millisecond wall clock derived from `sf->capture_wall_ns`; snapshot into `result` at publish time. Guard the `light_analyzer_` pointer.
- [ ] **Step 3: Build.** Run: `make build-engine-tests` (host compile of the changed headers/sources via the test target) and `make build` (full cross-compile + package). Expected: both succeed; no new warnings in changed files.
- [ ] **Step 4: Commit.** `feat(engine): run light analysis per frame and carry it in PipelineResult`

> Note: orchestrator threading is not unit-testable without hardware; correctness of the end-to-end path is verified by Task 8's integration test. This task's gate is a clean build plus the Task 1/2 unit tests still passing.

---

### Task 5: MQTT publisher emission + factory threading

**Files:**
- Modify: `engine/include/output/mqtt_publisher.h` (add `LightCfg light;` to `MqttPublisherCfg`; add members: `std::string light_topic_;`, `LightState last_light_{};`, `bool last_light_valid_{false};`, `int64_t last_light_pub_ms_{0};`; declare `void publish_light(const LightState&, const char* event, int64_t now_ms) noexcept;`)
- Modify: `engine/src/output/mqtt_publisher.cpp` (derive `light_topic_` in `start()` like `zone_event_topic_`; in `make_payload_locked()` append the scene `light` object and a per-track `"light":"<STATE>"` field when `cfg_.light.enabled`; in `publish_result()` store `r.light`/`r.light_enabled`, and when enabled + `on_change` + `light_changed(last,new)` call `publish_light(..., "change", now_ms)`; in `tick_publish()`/the tick loop, publish `"periodic"` when `now - last_light_pub_ms_ >= interval_seconds*1000`; `publish_light` uses `light_topic_json(...)` and `mosquitto_publish(..., qos=1, retain=true)`)
- Modify: `engine/include/output/publisher_factory.h` (add `const LightCfg& light = {}` param to `make_publisher` and `make_async_publishers`; set `mc.light = light`)
- Modify: `engine/src/orchestration/orchestrator.cpp` (pass `cfg_.light` as the new arg to `make_async_publishers`)
- Test: `engine/tests/test_publishers.cpp` (extend with any pure-logic assertions not already covered by Task 2, e.g. the interval-due predicate if extracted)

**Interfaces:**
- Consumes: `PipelineResult.light`/`light_enabled` (Task 4), `LightCfg` (Task 1), the Task 2 serialization helpers.
- Produces: messages on `.../light` and the added analytics fields (observed by Task 8).

- [ ] **Step 1: Thread the config.** Add the `LightCfg` param through the factory and the orchestrator call; add `cfg_.light` + the new members to the publisher; derive `light_topic_` in `start()` and bump the schema string to `analytics/v7.1` in `make_payload_locked()`.
- [ ] **Step 2: Emit.** Implement the scene object + per-track field (omitted entirely when `!cfg_.light.enabled`), `publish_light()` (retain=true, qos=1), the on-change path in `publish_result()`, and the periodic path in the tick loop.
- [ ] **Step 3: Tests.** If an interval-due helper (`bool light_due(int64_t now, int64_t last, int interval_s)`) is extracted, add it to `light_json`/analysis and assert it in `test_publishers.cpp` (or `test_light_json.cpp`). Run: `make run-engine-tests`. Expected: PASS.
- [ ] **Step 4: Build.** Run: `make build`. Expected: success.
- [ ] **Step 5: Commit.** `feat(engine): publish light/dark on new topic and in analytics`

---

### Task 6: Go config + configgen rendering

**Files:**
- Modify: `internal/config/config.go` (add `type LightConfig struct {...}` with JSON tags and `Enabled *bool` so omitted != explicit false; add `Light *LightConfig` to `Stream`; add defaults constants; add validation)
- Modify: `internal/configgen/configgen.go` (render a fully-populated `light` object into the engine config, applying all defaults, as the `zone` block does ~lines 53-78)
- Test: `internal/config/config_test.go`, `internal/configgen/configgen_test.go`

**Interfaces:**
- Consumes: the §4.5 defaults (kept in sync with the C++ `LightCfg`).
- Produces: a rendered `light` JSON object the engine's Task 3 parser reads.

- [ ] **Step 1: Write failing tests:**
  - `config_test.go`: `TestLightValidationRejectsBadRanges` (dark_luma>light_luma, ema_alpha<=0 or >1, interval_seconds<1, sample_stride<1, roi_frac<=0 or >1 each return an error); `TestLightOmittedIsValid` (nil `Light` passes validation).
  - `configgen_test.go`: `TestRenderLightDefaults` (stream with no `light` -> rendered engine config has a `light` object with every §4.5 default); `TestRenderLightOverrides` (operator sets `enabled=false`, `interval_seconds=30` -> rendered object has those plus defaults for the rest).
- [ ] **Step 2: Run, verify fail.** Run: `make run-tests`. Expected: FAIL (new tests).
- [ ] **Step 3: Implement** the struct, defaults, validation, and the `configgen` render block (fill every omitted field from defaults; `Enabled *bool` -> default `true` when nil).
- [ ] **Step 4: Run, verify pass.** Run: `make run-tests`. Expected: PASS.
- [ ] **Step 5: Commit.** `feat(watcher): per-stream light config + render into engine config`

---

### Task 7: Config samples and documentation

**Files:**
- Modify: `configs/config.json` (add a commented example `light` block to the stream entry / engine template)
- Modify: `configs/argus-config.generic.json` (document the `light` object with `_comment_*` keys, consistent with the file's style)
- Modify: `README.md` (document the `.../light` topic, the analytics `light` fields, and the `light` config block; update the MQTT/schema section; note schema v7.1)
- Modify: `docs/DESIGN.md` (add a short subsection or pointer to the light-detection spec, consistent with how other features are referenced)

**Interfaces:** none (documentation + samples).

- [ ] **Step 1: Update the two config samples** with the `light` block and comments; keep valid JSON (generic file uses `_comment_*` keys; `config.json` must stay parseable).
- [ ] **Step 2: Update README and DESIGN** with the topic, payloads (v7.1 scene + per-track), config reference, and the retain/qos note.
- [ ] **Step 3: Verify samples parse.** Run: `make run-tests` (the Go config tests load/round-trip sample-shaped configs). Expected: PASS.
- [ ] **Step 4: Commit.** `docs: document light/dark detection topic, fields, and config`

---

### Task 8: Integration test (file input, bright->dark and IR)

**Files:**
- Create: `test/light_detection_test.go` (follows `test/smoke_single_test.go` conventions; skips cleanly off-target when staged assets are absent, as the existing integration tests do)
- Create/obtain: small test media fixtures under `test/` (a short clip or synthetic frames that transition bright-colour -> dark, and a monochrome/IR-style clip). If generating is impractical, document the fixture requirement and gate the test on their presence.

**Interfaces:**
- Consumes: the running engine (file input), the local broker, the `.../light` topic and analytics `light` fields (Tasks 4-5).

- [ ] **Step 1: Write the integration test:** start the engine on a file-input stream via the supervisor path the smoke tests use; subscribe to `bs/argus/<id>/light` and `.../analytics`; assert (a) a retained `light` message arrives with a valid `state`; (b) a bright-colour segment yields `state==LIGHT`, `ir_mode==false`; (c) a dark/IR segment yields `state==DARK` (with `ir_mode==true` for the monochrome clip); (d) analytics messages carry `light` at scene level and on each track when people are present.
- [ ] **Step 2: Run.** Run: `make run-integration-tests` (and/or `make run-tests` if wired there). Expected: PASS on-target / SKIP cleanly off-target.
- [ ] **Step 3: Commit.** `test: integration coverage for light/dark detection`

---

## Self-Review

**Spec coverage:** algorithm §4 -> Task 1; "how light" measure §4.4 -> Task 1 (`level`) + Task 2 (serialization); new `.../light` topic §7.1 (periodic + on-change + retain) -> Tasks 2,5; analytics scene+per-track fields §7.2 -> Tasks 2,5; config §4.5/§8 -> Tasks 3 (engine), 6 (Go), 7 (samples/docs); data flow §5 -> Task 4; tests §10 -> Tasks 1,2,3,6 (unit) + 8 (integration). All sections mapped.

**Review Focus:** each of the five lines has an owning task with a named test (tiny/empty frame, overflow, invalid config, monochrome mode, flapping) — Task 1 and Task 6.

**Type consistency:** `LightCfg`, `LightState`, `LightLevelState`, `LightAnalyzer::update/snapshot`, `light_state_str`, `light_scene_json`, `light_topic_json`, `light_changed` are defined once (Tasks 1-2) and referenced by the same names in Tasks 4-5. `PipelineResult.light`/`light_enabled`, `PipelineConfig.light`, `MqttPublisherCfg.light`, and the factory `light` param are consistent across Tasks 4-5.

**Proportion:** the plan carries signatures, exact values, and test names rather than function bodies (the one algorithm-shaped step, Task 1 Step 4, is described in prose with formulas, not transcribed code). Length is proportional to the spec.
