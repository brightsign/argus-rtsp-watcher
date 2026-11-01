# Getting live bounding-box data over MQTT

This guide covers how to consume the per-person **bounding boxes** (and gaze/face boxes) that Argus RTSP Watcher publishes for every camera, in real time, over MQTT.

If you only want the box coordinates for an overlay or analytics, this is the document you need. It describes the broker, the topics, the exact message schema, the coordinate system, and working consumer code.

## The short version

- **Broker:** the player runs a mosquitto broker on TCP **1883**, anonymous (no auth), plain MQTT 3.1.1, **QoS 0**, not retained.
- **Topics:** one per stream — `bs/argus/<stream-id>/analytics`. Subscribe to all with `bs/argus/#`.
- **Rate:** about one message per second per stream (`period_ms: 1000`).
- **Boxes:** in the `tracks[]` array. `tracks[i].bbox` is the **person** box; `tracks[i].gaze.face_bbox` is the **face** box (present when a face was matched to that person).
- **Coordinates:** pixels in the source frame, `[x0, y0, x1, y1]` (top-left, bottom-right), origin top-left. The frame size is the top-level `frame_w` × `frame_h`.

```sh
mosquitto_sub -h <player-ip> -t 'bs/argus/#' -v
```

## Data flow

```mermaid
flowchart LR
    ENG["argus-engine (per stream)\nYOLOX + RetinaFace + ByteTrack"] -->|publishes JSON| BRK["mosquitto :1883"]
    BRK -->|bs/argus/&lt;id&gt;/analytics| SUB["your MQTT client"]
    SUB --> APP["overlay / analytics / storage"]
```

Each stream's engine detects people (YOLOX), detects faces and gaze (RetinaFace + landmark geometry), tracks people across frames (ByteTrack), and once per second publishes the current tracks to that stream's topic.

## Topics

The stream `id` from `config.json` becomes the topic segment. For the example config:

| Stream `id` | Topic |
|-------------|-------|
| `lobby` | `bs/argus/lobby/analytics` |
| `entrance` | `bs/argus/entrance/analytics` |
| `aisle` | `bs/argus/aisle/analytics` |

Wildcards: `bs/argus/#` (everything) or `bs/argus/+/analytics` (all analytics topics). The message also carries `device` and `stream` fields so a wildcard subscriber can still tell cameras apart.

## Message schema (`analytics/v7.0`)

One JSON object per message. Annotated example (two people, one looking at the display):

```json
{
  "schema": "analytics/v7.0",
  "ts": 164.68,
  "device": "aisle",
  "stream": "aisle",
  "frame_seq": 48213,
  "frame_ts": 1735689123456,
  "frame_w": 1024,
  "frame_h": 576,
  "model": "yolox_s",
  "fw_version": "7.0.0",
  "npu_load": 42.0,
  "people": 2,
  "people_confident": 2,
  "gaze": 1,
  "fps": 10,
  "scene": { "person_count": 2, "employee_count": 0, "customer_count": 0, "unknown_uniform_count": 2 },
  "roi": { "type": "border", "border_frac": 0.30, "rect": [307, 172, 716, 403] },
  "health": { "detector_fps": 10.0, "tracker_fps": 10.0, "queue_latency_ms": 0, "dropped_frames": 0, "last_model_reload_ts": 0.0 },
  "tracks": [
    {
      "id": 63,
      "state": "Confirmed",
      "bbox": [52.9, 227.4, 286.6, 560.0],
      "score": 0.93,
      "zones": ["roi"],
      "dir": "UR", "deg": 41.0, "dir_conf": 0.80,
      "speed": 12.3, "speed_norm": 0.012,
      "dwell": 31.03, "enter": false, "exit": false,
      "uniform": { "class": "unknown", "confidence": 0.0, "is_employee": false },
      "gaze": { "detected": 1, "time": 8.00, "face_bbox": [231.0, 167.0, 240.0, 180.0] }
    },
    {
      "id": 71,
      "state": "Confirmed",
      "bbox": [640.0, 210.0, 820.0, 540.0],
      "score": 0.88,
      "zones": [],
      "dir": "?", "deg": 0.0, "dir_conf": 0.0,
      "speed": 0.0, "speed_norm": 0.0,
      "dwell": 4.10, "enter": true, "exit": false,
      "uniform": { "class": "unknown", "confidence": 0.0, "is_employee": false }
    }
  ]
}
```

Note the second track has **no `gaze` object** — the `gaze` key is present only when a face was matched to that person this frame.

### Top-level fields

| Field | Type | Meaning |
|-------|------|---------|
| `schema` | string | Always `analytics/v7.0`. |
| `ts` | float | Timestamp in seconds (monotonic since engine start). |
| `device` | string | The stream `id` (from `config.json`). |
| `stream` | string | Stream identifier. |
| `frame_seq`, `frame_ts` | int | **Video/MQTT sync key.** `frame_seq` is the capture sequence number burned into the matching annotated frame's stamp; `frame_ts` is that frame's capture wall-clock time, epoch milliseconds. See the "Frame stamp" section below. |
| `frame_w`, `frame_h` | int | **Source frame dimensions in pixels — the coordinate space for all boxes.** |
| `model` | string | Detector model name. |
| `fw_version` | string | Schema/firmware version. |
| `npu_load` | float | Approximate NPU load (%). |
| `people` | int | Number of tracks in this message (= `tracks.length`). |
| `people_confident` | int | Tracks with `score >= 0.70`. |
| `gaze` | int | Number of people currently looking at the display. |
| `fps` | int | Processing frame rate. |
| `roi` | object | Region of interest: a border inset (`border_frac`) and its pixel `rect` `[x0,y0,x1,y1]`. |
| `health` | object | `detector_fps`, `tracker_fps`, `queue_latency_ms`, `dropped_frames`, `last_model_reload_ts`. |
| `scene` | object | Person/employee/customer counts. **Vestigial in this build** — employee/customer classification was dropped, so `employee_count`/`customer_count` are always 0 and everyone is `unknown`. |
| `tracks` | array | One entry per detected person (see below). |

### Per-track fields (`tracks[]`)

| Field | Type | Meaning |
|-------|------|---------|
| `id` | int | **Stable track ID** — the same person keeps the same id across frames. |
| `state` | string | Tracker state (`Confirmed`). |
| **`bbox`** | `[x0,y0,x1,y1]` float | **Person bounding box**, pixels, top-left to bottom-right. |
| `score` | float | Detection confidence (0–1). |
| `zones` | string[] | Zones the person is in, e.g. `["roi"]`. |
| `dir` | string | 8-way compass direction of movement (`R,UR,U,UL,L,DL,D,DR`, or `?`). |
| `deg` | float | Direction in degrees. |
| `dir_conf` | float | Direction confidence (0–1). |
| `speed` | float | Speed in pixels/second. |
| `speed_norm` | float | Speed normalized to the frame diagonal. |
| `dwell` | float | Seconds the person has been tracked in the area. |
| `enter` | bool | True on the frame the person entered. |
| `exit` | bool | True on the frame the person left. |
| `uniform` | object | `{class, confidence, is_employee}`. **Vestigial** — always `{"unknown", 0.0, false}` in this build. |
| `gaze` | object? | Present only when a face matched this person. `detected` (1 = looking at display), `time` (accumulated seconds of gaze), and **`face_bbox`** `[x0,y0,x1,y1]` — the **face bounding box**, pixels. |
| `in_zone` | bool | Present only when the stream has a custom `zone` configured. `true` when this person is in the zone under the stream's `zone_mode`: `any` (default — any part of the box overlaps the zone) or `base` (box feet/bottom-center inside). Additive to `zones`. |

## The bounding boxes, precisely

Two boxes per person, both `[x0, y0, x1, y1]` in **source-frame pixels**:

- **Person box** — `tracks[i].bbox`. Always present.
- **Face box** — `tracks[i].gaze.face_bbox`. Present only when `tracks[i].gaze` exists (a face was matched). Use `tracks[i].gaze.detected == 1` to know if that face is looking at the display.

Coordinate system:

```
(0,0) ── x ──▶ (frame_w, 0)
  │        ┌───────────────┐  (x0,y0) top-left
  y        │    person     │
  │        └───────────────┘  (x1,y1) bottom-right
  ▼
(0, frame_h)
```

- Origin is the top-left of the frame.
- `x` grows right to `frame_w`; `y` grows down to `frame_h`.
- `(x0,y0)` is the top-left corner, `(x1,y1)` the bottom-right. Width = `x1 - x0`, height = `y1 - y0`.

### Scaling to a display

If you render the video at some `displayW × displayH` (different from `frame_w × frame_h`), scale each coordinate:

```
sx = displayW / frame_w
sy = displayH / frame_h
drawRect(x0*sx, y0*sy, (x1-x0)*sx, (y1-y0)*sy)
```

Use `frame_w`/`frame_h` from the same message — don't hard-code, since different cameras have different resolutions.

## Frame stamp (video ↔ MQTT sync)

The boxes are not in the video: an annotated frame only has boxes *drawn* on it, and the structured boxes come over MQTT. To relate a box to a frame of a recorded video, use the timing fields of the MQTT message, below. The annotated frames also carry a human-readable time stamp so you can see that time is advancing.

### The MQTT fields

A message is built once per second from the **latest inference result**, not from the newest captured frame, so two different frames are named:

| Field | Type | Meaning |
|-------|------|---------|
| `frame_seq` | int | Sequence number of the **newest frame captured** when the message was built — the value burned into that frame's stamp (below). Monotonically increasing per stream. |
| `frame_ts` | int | That frame's capture time as the engine saw it, **epoch milliseconds** (`CLOCK_REALTIME`, not monotonic uptime — comparable across restarts and hosts). Taken *after* network, proxy and decode delay. |
| `det_seq` | int | Sequence number of the frame **the person boxes were detected on**. It lags `frame_seq` by the inference delay (a few frames normally, more under CPU load). `0` = no detection frame yet. Two consecutive messages can carry the same `det_seq` if no new frame was inferred between them. |
| `det_ts` | int | That detection frame's engine capture time, epoch ms. |
| `source_ts` | int or `null` | The **camera's own capture time** of the detection frame, epoch ms, recovered from the stream's RTCP sender reports. `null` when the camera sent none or its clock is not plausibly in sync with the engine's (more than 60 s apart). |

**To relate boxes to a frame of a recorded video, match on `source_ts` when it is present, otherwise `det_ts` — not `frame_ts`/`frame_seq`.** The `frame_*` pair names a newer frame than the one the boxes belong to. `source_ts` is the best match to a recording of the camera's own stream, because it is the camera's clock, not the engine's. Match with a tolerance of half a frame interval, and keep the camera, the player and the recorder on the same NTP source — clock agreement limits the match more than anything else.

These are cheap top-level metadata and are always present, independent of `enable_frame_stamp` (the flag below only controls whether the visual stamp is drawn on the frame itself).

#### Reading `source_ts` correctly

`source_ts` comes from RTCP sender reports, which pair an RTP timestamp with the sender's wall clock. If the engine reads the stream through a proxy such as MediaMTX, the "sender" it sees may be the **proxy**, whose clock reads when it received each packet, not when the camera captured it. A `lag` (the frame's engine capture time minus `source_ts`; see the [README](../README.md#the-time-stamp-on-the-preview-and-what-lag-means)) that is always a few milliseconds is the sign of that; a `lag` of tens to hundreds of milliseconds is the camera's real capture-to-engine delay. Run with `log_level: debug` and read `camera_time=` and `last_lag_ms=` on the `capture_loop` line to see which you have. Camera time applies to RTSP inputs only, and needs GStreamer 1.22 or later in the engine.

### The stamp burned into the frame

When `enable_frame_stamp` is true (default) and `enable_frame_output` is on, the engine burns one line of text into the lower-left corner of each annotated frame: `#<frame_seq>  <ISO-8601 UTC time, ms precision>` (e.g. `#48213  2024-12-31T23:52:03.456Z`), white on a filled dark background, pinned to the very bottom of the frame. It is for a person to see that time is advancing in a clip.

When the camera's capture time is known the line ends with the lag of that frame and its running average over the last 15 seconds, e.g. `#48213  2024-12-31T23:52:03.456Z  lag 55ms  avg15s 61ms`. What the lag is, exactly, and what it leaves out, is defined in the [README](../README.md#the-time-stamp-on-the-preview-and-what-lag-means). The text scales with frame height so it reads the same at any resolution, and its format is built by `format_frame_stamp_text()` in `engine/include/orchestration/frame_stamp.h`.

The stamp is not meant to be decoded by a program. The frame's identity for machine use is in the MQTT fields above (`det_seq`, `det_ts`, `source_ts`), which are matched against a recording by time, not read back out of the image.

### Config

`enable_frame_stamp` (default `true`) in the engine config controls only the burned-in text stamp; it has no effect on the always-present `frame_seq`/`frame_ts`/`det_seq`/`det_ts` MQTT fields.

`enable_camera_time` (default `true`) in the engine config controls whether each RTSP frame's camera capture time is recovered from RTCP sender reports. Turn it off to stop populating `source_ts` (it then stays `null`) and to drop the `lag` and `avg15s` suffixes from the stamp. It only adds metadata; it does not change how the stream is decoded.

## Custom zone

When you add a `zone` field to a stream in `config.json`, each track will report an additional `in_zone` boolean. The zone is specified as `[x0,y0,x1,y1]` with all values in the range `[0,1]` (normalized frame coordinates), e.g., `"zone": [0.146, 0.104, 0.781, 0.972]`. This is useful for counting people in an "important area" (checkout queue, parking space, aisle) without setting up separate ROI engines.

An optional per-stream `zone_mode` selects how `in_zone` is decided:

- `"any"` (default): `true` when **any part** of the person's bounding box overlaps the zone.
- `"base"`: `true` only when the person's **feet** (box bottom-center) are inside the zone.

A few optional per-stream fields control how the zone is drawn and counted on the annotated frame:

- `zone_color` — outline color as a `#RRGGBB` hex string (default `#ffffff`, white).
- `zone_thickness` — outline width in pixels, `1`–`10` (default `3`).
- `zone_border_thickness` — red alert-border width in pixels, `1`–`100` (default `20`), independent of the outline width.
- `zone_window_minutes` — rolling window for the on-frame detect counter, `1`–`1440` (default `15`).

```json
{ "id": "checkout", "rtsp_url": "rtsp://…", "zone": [0.146, 0.104, 0.781, 0.972], "zone_mode": "base", "zone_color": "#ff8800", "zone_thickness": 4, "zone_window_minutes": 15 }
```

Editing any of these in `config.json` triggers a live restart of only that camera's engine — no need to reboot the extension. The on-player **zone dashboard** (port `20300`) lets you draw the zone on each camera's live view, flip the any/base toggle, pick the outline color and line width, set the count window, and Save per pane. The companion `tools/boxstats` tool can answer offline area-range questions from recorded video.

### On-frame detect counter and red alert border

The engine draws two things on the annotated frame (the preview feed and the video walls) for a stream with a zone:

- **Detect counter** — the number of distinct in-zone detection episodes in the last `zone_window_minutes`, drawn in the upper-right corner in the zone's color, at the frame-stamp text size. A new episode is counted when the zone goes from empty to occupied; continuous presence counts once.
- **Red alert border** — a red border (width = `zone_border_thickness`, independent of the outline) burned around the whole frame while the zone is triggered. It fires on the first in-zone frame and holds solid across brief detection gaps, clearing shortly after the trigger condition clears. The bundled video walls (`presentation/`, `presentation2/`) simply display the feeds, so this border appears there automatically — there is no browser-side flashing.

### Low-latency in-zone event (`zone_event`)

The per-track `in_zone` field above rides the regular analytics message, which is throttled to one publish per second. For external consumers that want a fast signal, a stream with a zone also publishes a dedicated event on a sibling topic, **`bs/argus/<id>/zone_event`**, the moment **two in-zone detections land within a rolling 1-second window** (using the same `any`/`base` predicate). It then repeats at most every 500 ms while in-zone activity continues, and stops when it ends. This bypasses the 1 Hz analytics tick, so consumers react within a frame or two instead of up to a second.

```json
{ "device_id": "checkout", "event": "in_zone", "in_zone": 2, "mode": "base", "ts_ms": 1733520000123 }
```

`in_zone` is the number of tracks inside the zone on the firing frame; `ts_ms` is a monotonic millisecond timestamp. (The in-frame red border is driven independently by the engine, not by this topic.)

## Consuming the data

### Command line

```sh
# everything, human-readable
mosquitto_sub -h <player-ip> -t 'bs/argus/#' -v

# one camera, pipe through jq to pull just the boxes
mosquitto_sub -h <player-ip> -t 'bs/argus/lobby/analytics' \
  | jq -c '.tracks[] | {id, bbox, gaze: .gaze.face_bbox, looking: .gaze.detected}'
```

The `mosquitto_sub`/`mosquitto_pub` binaries are also bundled in the extension at `/var/volatile/bsext/ext_npu_watch/<SOC>/bin/`.

### Python (paho-mqtt)

```python
import json
import paho.mqtt.client as mqtt

def on_message(_client, _userdata, msg):
    m = json.loads(msg.payload)
    cam = m["device"]
    fw, fh = m["frame_w"], m["frame_h"]
    for t in m["tracks"]:
        person = t["bbox"]                     # [x0, y0, x1, y1]
        gaze = t.get("gaze")                   # None if no face matched
        face = gaze["face_bbox"] if gaze else None
        looking = bool(gaze and gaze["detected"])
        print(f"{cam} id={t['id']} person={person} face={face} looking={looking} "
              f"(frame {fw}x{fh})")

c = mqtt.Client()
c.on_message = on_message
c.connect("<player-ip>", 1883, keepalive=30)
c.subscribe("bs/argus/#", qos=0)
c.loop_forever()
```

### Node.js (mqtt)

```js
const mqtt = require("mqtt");
const client = mqtt.connect("mqtt://<player-ip>:1883");

client.on("connect", () => client.subscribe("bs/argus/#"));
client.on("message", (topic, payload) => {
  const m = JSON.parse(payload.toString());
  for (const t of m.tracks) {
    const person = t.bbox;                         // [x0,y0,x1,y1]
    const face = t.gaze ? t.gaze.face_bbox : null;
    const looking = !!(t.gaze && t.gaze.detected);
    console.log(m.device, t.id, person, face, looking, m.frame_w, m.frame_h);
  }
});
```

### Browser overlay (canvas over video)

Draw the boxes on a `<canvas>` layered over a video element (e.g. the RTSP stream via WebRTC/HLS, or the built-in MJPEG preview). Use a browser MQTT client over WebSockets, or relay MQTT→WebSocket from your app server.

```js
// video: the <video>/<img> element; canvas: an overlay sized to match it
function draw(m) {
  const sx = canvas.width / m.frame_w;
  const sy = canvas.height / m.frame_h;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  for (const t of m.tracks) {
    const [x0, y0, x1, y1] = t.bbox;
    ctx.strokeStyle = t.gaze && t.gaze.detected ? "lime" : "white";
    ctx.lineWidth = 2;
    ctx.strokeRect(x0 * sx, y0 * sy, (x1 - x0) * sx, (y1 - y0) * sy);
    ctx.fillText(`#${t.id} dwell ${t.dwell.toFixed(1)}s`, x0 * sx, y0 * sy - 4);
    if (t.gaze && t.gaze.face_bbox) {
      const [fx0, fy0, fx1, fy1] = t.gaze.face_bbox;
      ctx.strokeStyle = "cyan";
      ctx.strokeRect(fx0 * sx, fy0 * sy, (fx1 - fx0) * sx, (fy1 - fy0) * sy);
    }
  }
}
```

If you just want boxes-over-video without writing any of this, the extension already renders it — see the browser preview in the [README](../README.md#outputs).

## Notes and gotchas

- **Data only flows when the engines are running.** If `curl :9200/healthz` shows a stream's `Restarts` climbing, its engine can't open the RTSP source and nothing is published for it. Fix the stream first (see the README troubleshooting table).
- **QoS 0, not retained.** A subscriber that connects sees only messages published from then on — there's no last-value replay. Subscribe before you need the data.
- **The `gaze` object is optional per track.** Always null-check it before reading `face_bbox`/`detected`.
- **`uniform` and `scene` employee/customer fields are vestigial** in this person+gaze build — always default. Ignore them unless a future build re-adds the classifier.
- **Coordinates are per-message.** Read `frame_w`/`frame_h` from the same message you're drawing; cameras differ in resolution.
- **One broker, all streams.** All cameras publish to the single broker on the player; the topic's `<id>` segment is how you separate them.

## Related

- [README.md](../README.md) — overview, config, deploy.
- [docs/DESIGN.md](DESIGN.md) — how the pipeline produces these results.
