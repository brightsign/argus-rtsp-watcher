# Plan: Nest light fields under a `light` object on the `/light` MQTT topic

Spec: `docs/superpowers/specs/2026-10-28-mqtt-light-object-design.md`
Build branch: `build/mqtt-light-object`

All build/test via the Makefile (`make build`, `make run-tests`) per project
convention. TDD: update the failing expectation first, then the implementation.

## Task 1 — C++ unit test: assert the nested `/light` shape (RED)

In `engine/tests/test_light_json.cpp`, update `TopicJsonShape` to expect:

```
{"device_id":"cam-01","event":"change","ts_ms":1700000000123,"light":{"state":"LIGHT","level":73,"ir_mode":false,"luma":120.0,"chroma":40.5}}
```

Keep `SceneJsonShape` (unchanged — analytics object), `StateStrings`,
`light_changed` tests, and `OversizedDeviceIdTruncatesInsteadOfThrowing`
(still expects 511-byte truncation).

## Task 2 — C++ implementation: wrap `light_scene_json` (GREEN)

In `engine/src/analysis/light_json.cpp`, rewrite `light_topic_json` to emit the
envelope with the scene object embedded as a `%s` argument:

```
{"device_id":"%s","event":"%s","ts_ms":%lld,"light":%s}
```

where the last `%s` is `light_scene_json(state)`. `device_id`/`event` stay
`%s` arguments. Preserve the `finish()` truncation guard and 512-byte buffer.

## Task 3 — Go integration test: nested decoder (RED->GREEN together)

In `test/light_detection_test.go`:
- Split `lightMessage` so the measurement fields move into a nested
  `*lightScene` under `json:"light"`; keep `device_id`, `event` top-level.
- Update accesses: `light.State`->`light.Light.State`,
  `light.IRMode`->`light.Light.IRMode`, add a nil guard on `light.Light`.
- `assertValidLightMessage` validates `light.Light != nil` before field checks;
  `light.Event` check stays top-level.

(Integration tests are `//go:build integration`, on-target only; they must still
compile and pass `go vet` via `make`.)

## Task 4 — Docs

In `README.md`, update the `bs/argus/<id>/light` example payload (~L410) to the
nested shape. Leave the `analytics/v7.1` object description (~L428) unchanged.

## Edge-case matrix (test-as-guardrails)

- Oversized device id -> truncates to 511 bytes, no throw (existing test).
- `UNKNOWN` state -> inner object still well-formed (covered by scene json path).
- `ir_mode` true/false both render as bare JSON booleans (existing scene test).
- Envelope fields never collide with inner object keys (distinct namespaces).

## Verification

- `make build` (C++ engine + Go) green.
- `make run-tests` green (host-runnable C++ unit tests incl. light_json).
- `make check` / vet clean.
- Manual: re-sample live `bs/argus/#` from the player only after the engine is
  rebuilt and deployed (out of scope for this branch; code+tests are the
  deliverable here).
