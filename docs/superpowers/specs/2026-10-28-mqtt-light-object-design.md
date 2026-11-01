# Design Spec: Nest light fields under a `light` object on the `/light` MQTT topic

Date: 2026-10-28
Status: Approved (design decisions made interactively; see Decisions)

## Problem

The standalone per-stream MQTT topic `bs/argus/<id>/light` emits its five
measurement fields (`state`, `level`, `ir_mode`, `luma`, `chroma`) **flat**,
mixed in with the message envelope (`device_id`, `event`, `ts_ms`):

```json
{"device_id":"back-door","event":"periodic","state":"DARK","level":10,
 "ir_mode":false,"luma":25.9,"chroma":0.2,"ts_ms":1793191084975}
```

Consumers want the measurement grouped into a single object so the scene
reading is one addressable unit, distinct from the delivery envelope.

## Decisions

1. **Object name: `light`** (not `luma`). Naming the object `luma` while it also
   carries a scalar `luma` reading produces an awkward `luma.luma`. Using `light`
   keeps the inner scalar `luma` intact and matches the key already used for the
   same object inside the analytics message.
2. **Clean break**, not additive. The flat fields are removed from the `/light`
   topic payload and replaced by the nested object. No transition period with
   both shapes.

## Key realization

The analytics message **already** embeds this exact object under the key
`"light"` (produced by `light_scene_json`). Therefore:

- The analytics message does **not** change. Its schema stays `analytics/v7.1`.
- Only the standalone `/light` topic payload changes.
- `light_topic_json` can wrap `light_scene_json`'s output, giving one source of
  truth for the object's shape across both topics (DRY).

## Target shape

```json
// bs/argus/<id>/light  (AFTER)
{"device_id":"back-door","event":"periodic","ts_ms":1793191084975,
 "light":{"state":"DARK","level":10,"ir_mode":false,"luma":25.9,"chroma":0.2}}
```

Envelope fields (`device_id`, `event`, `ts_ms`) stay at top level. The inner
`light` object is byte-for-byte identical to what the analytics message carries.

## Non-goals

- No change to the analytics message or its schema version.
- No change to topic names, QoS, retain, or publish cadence.
- No change to the light-detection algorithm or config.

## Security considerations

- The emitter is a `snprintf` into a fixed 512-byte buffer with the existing
  `finish()` truncation guard (CWE-120 bounded). Wrapping one small fixed object
  does not threaten the bound; the oversized-device-id truncation test still
  applies.
- `device_id` and `event` remain `%s` **arguments**, never the format string
  (CWE-134). The embedded `light_scene_json` result is likewise passed as a
  `%s` argument, not interpolated into the format string.

## Blast radius

- `engine/src/analysis/light_json.cpp` — `light_topic_json` rewritten to wrap
  `light_scene_json`. `light_scene_json` unchanged.
- `engine/tests/test_light_json.cpp` — `TopicJsonShape` expected string updated;
  truncation test preserved.
- `test/light_detection_test.go` — `lightMessage` gains a nested scene struct;
  field accesses updated.
- `README.md` — `/light` payload example updated; analytics section unchanged.
