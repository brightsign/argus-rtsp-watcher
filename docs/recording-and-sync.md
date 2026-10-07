# Recording the MJPEG streams and matching them to the bounding-box data

This guide shows how to (1) read and record the annotated MJPEG video the player serves for each stream, (2) record the bounding-box/analytics data from MQTT, and (3) join a recorded video frame to the exact MQTT message it came from, using the per-frame **sync stamp** (`frame_seq` / `frame_ts`).

The join is exact — not clock-aligned guessing — because every annotated frame carries a stamp of the same `frame_seq` that its MQTT message carries.

## The two streams and how they line up

For each camera the player exposes two independent outputs:

| Output | Where | Rate | Carries |
|--------|-------|------|---------|
| **Annotated MJPEG** | `http://<player>:<port>/video` | ~detector fps | video frames with boxes drawn on them, each stamped with `frame_seq` |
| **Analytics (MQTT)** | `bs/argus/<id>/analytics` on `<player>:1883` | ~`period_ms` (default 1/s) | `frame_seq`, `frame_ts`, and the structured `tracks[]` (person/face boxes) |

`port = 20200 + the stream's 0-based index in config.json` (so `lobby`→20200, `entrance`→20201, `aisle`→20202 for the sample config). Also `http://<player>:<port>/image` = latest single JPEG snapshot.

**The join key is `frame_seq`** (a monotonic per-capture counter). Each MQTT message names the `frame_seq` of the frame its boxes came from; that same number is burned into the matching video frame's stamp. Match them and you have structured boxes attached to a specific recorded frame. `frame_ts` (epoch-ms wall clock) is a secondary, human-friendly key.

> Rate note: MQTT publishes ~once per second by default (`period_ms: 1000`), while the video runs at the detector fps. So out of the box you get box data for roughly **one video frame per second** — the frames whose `frame_seq` appears in MQTT. To get boxes for (nearly) every frame, lower `publishers[].mqtt.period_ms` in the engine config toward `1000 / fps`.

## 1. Record the MJPEG video

`ffmpeg` reads the `/video` MJPEG endpoint directly. Two useful forms:

Record the whole stream to a container (keeps the JPEG frames, `-c copy`):

```sh
# one per stream; run in parallel
ffmpeg -use_wallclock_as_timestamps 1 -i http://<player>:20200/video -c copy lobby.mkv
ffmpeg -use_wallclock_as_timestamps 1 -i http://<player>:20201/video -c copy entrance.mkv
ffmpeg -use_wallclock_as_timestamps 1 -i http://<player>:20202/video -c copy aisle.mkv
```

Or dump individual stamped JPEG frames (simplest for decoding the stamp per frame):

```sh
mkdir -p lobby_frames
ffmpeg -i http://<player>:20200/video -q:v 2 lobby_frames/f_%08d.jpg
```

Each written frame is a full JPEG with the stamp in its lower-left corner. (Segment long recordings with `-f segment -segment_time 300 -strftime 1 lobby_%Y%m%d-%H%M%S.mkv`.) Or just use `tools/record-frames.sh`, which dumps the frames folder and records the MQTT boxes at the same time.

## 2. Record the MQTT analytics

Log each message as one JSON line (JSONL), with the receive time for convenience:

```sh
mosquitto_sub -h <player> -t bs/argus/lobby/analytics \
  | while IFS= read -r line; do printf '%s %s\n' "$(date +%s.%N)" "$line"; done > lobby.boxes.jsonl
```

Or use the example client with `--raw`:

```sh
./example-client/bin/argus-example-client --broker <player>:1883 --topic bs/argus/lobby/analytics --raw > lobby.boxes.jsonl
```

Each line contains `frame_seq`, `frame_ts`, `frame_w`/`frame_h`, and `tracks[]` (each with `bbox` and optional `gaze.face_bbox`). See [bounding-boxes.md](bounding-boxes.md) for the full schema.

## 3. The sync stamp — how to read `frame_seq` off a video frame

> Ready-made: the `tools/` directory implements all of this. `tools/record-frames.sh` does steps 1–2 (records the frames folder and `boxes.jsonl` together), and `tools/framebox` does steps 3–4 (decode a frame's stamp and print its boxes). The rest of this section documents the format so you can build your own or verify theirs.

Every annotated frame has two representations of the same value, stacked in the **lower-left corner**:

- **Human-readable** (pinned to the very bottom): `#<frame_seq>  <ISO-8601 UTC time>` — for eyeballing.
- **Machine-readable** (directly above the text): a grid of solid black/white cells encoding the 64-bit `frame_seq`, for programmatic decoding.

### Machine-readable layout (exact)

Constants come from `engine/include/orchestration/frame_stamp.h` / `engine/src/orchestration/inference_worker.cpp` — read those if this and the code ever disagree. The whole stamp **scales with frame height `H`** by an integer factor so it reads the same at any resolution, and it's **bottom-anchored**:

```
# each dimension scales continuously with H (integer round-half-up), floored at its base:
cell      = max(10, round(10 * H / 600))   # e.g. 10 @576, 12 @720, 17 @1024, 32 @1920
margin    = max(4,  round( 4 * H / 600))
text_band = max(28, round(28 * H / 600))   # reserved bottom band for the human line
```

- **Text band:** the bottom `text_band` px hold the human-readable line (baseline at `H - margin`); the grid sits above it.
- **Quiet zone:** a solid black rectangle from `(margin, H - text_band - (9+2)*cell)` to `(margin + (8+2)*cell, H - text_band)`, one cell thick on every side of the grid.
- **Bit grid:** `8` columns × `9` rows of `cell×cell` px cells. Cell `[0,0]`'s top-left corner is one cell in from the quiet zone, at `(margin + cell, grid_y0)` where `grid_x0 = margin + cell` and `grid_y0 = H - text_band - (9+1)*cell`.
- **Cell color:** pure **black = bit 0**, pure **white = bit 1**.
- **Bit order:** rows `0..7` hold the 8 bytes of `frame_seq`, **MSB-first** (row 0 = bits 63–56, row 7 = bits 7–0). Row `8` holds an **8-bit XOR checksum** of those 8 bytes. Within a row, column 0 = that byte's MSB (bit 7) … column 7 = its LSB (bit 0).
- **Sampling:** read the pixel at each cell's center — cell `[row,col]` center is at `(grid_x0 + col*cell + cell//2, grid_y0 + row*cell + cell//2)`; threshold luma at ~127.

`cell` is ≥ 10 px (scale ≥ 1) so cells survive JPEG recompression.

### Python decoder

```python
import cv2  # or use PIL

BASE_CELL, BASE_MARGIN, BASE_TEXTBAND, COLS, ROWS, SCALE_REF = 10, 4, 28, 8, 9, 600

def decode_frame_seq(img_bgr):
    """Return (frame_seq, ok) decoded from a stamped frame, ok=False if checksum fails."""
    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)
    h = gray.shape[0]
    def sdim(base):  # engine's scaled(base) = max(base, round(base*H/600))
        return max(base, (h * base + SCALE_REF // 2) // SCALE_REF)
    cell, margin, textband = sdim(BASE_CELL), sdim(BASE_MARGIN), sdim(BASE_TEXTBAND)
    gx0 = margin + cell
    gy0 = h - textband - (ROWS + 1) * cell   # grid y origin, above the text band
    off = cell // 2
    rows = []
    for r in range(ROWS):
        byte = 0
        for c in range(COLS):
            x = gx0 + c * cell + off
            y = gy0 + r * cell + off
            bit = 1 if gray[y, x] > 127 else 0   # white=1, black=0
            byte = (byte << 1) | bit             # col 0 = MSB
        rows.append(byte)
    seq_bytes, checksum = rows[:8], rows[8]
    calc = 0
    for b in seq_bytes:
        calc ^= b
    ok = (calc == checksum)
    seq = 0
    for b in seq_bytes:                          # row 0 = MSB byte
        seq = (seq << 8) | b
    return seq, ok
```

`ok=False` means the read is unreliable (blur, cropping, heavy compression, or the stamp was disabled) — skip that frame rather than trusting it.

## 4. Join video frames to box data

For a single frame, `tools/framebox` does the whole join — decode the stamp and print (or emit as JSON) the boxes recorded for it:

```sh
cd tools && make build
./bin/framebox --boxes recordings/lobby-.../boxes.jsonl recordings/lobby-.../frames/f_00000123.jpg
```

To join a whole recording yourself, match on `frame_seq` — with per-frame JPEGs and a JSONL of MQTT messages:

```python
import json, glob, cv2

# index the box data by frame_seq
boxes_by_seq = {}
for line in open("lobby.boxes.jsonl"):
    line = line.strip()
    payload = line.split(" ", 1)[1] if line[:1].isdigit() else line  # drop optional leading recv-timestamp
    m = json.loads(payload)
    boxes_by_seq[m["frame_seq"]] = m

# walk the recorded frames; when a frame's stamp matches a message, we have a pair
for path in sorted(glob.glob("lobby_frames/*.jpg")):
    img = cv2.imread(path)
    seq, ok = decode_frame_seq(img)
    if not ok:
        continue
    msg = boxes_by_seq.get(seq)
    if msg is None:
        continue          # this frame had no published boxes (MQTT is ~1/s)
    # msg["tracks"] holds the structured boxes for THIS exact frame:
    for t in msg["tracks"]:
        x0, y0, x1, y1 = t["bbox"]           # already in this frame's pixel space
        # ... draw, crop, label, export a dataset row, etc.
    print(path, "seq", seq, "people", msg["people"])
```

Notes on the join:
- **Coordinates need no scaling.** The MJPEG frame *is* the source frame the boxes were computed on, so `bbox`/`face_bbox` are already in the recorded frame's pixel space (`frame_w × frame_h`). Only scale if you later resize the video.
- **The `/video` frames already have boxes drawn on them.** The join is for attaching the *structured* data (ids, gaze, dwell, scores) to specific frames — e.g. building a labeled dataset, cropping per-person, or analytics. If you want boxes on *raw* (un-annotated) video, that isn't exposed today (it would need a raw-frame writer in the engine).
- **Going the other way** (overlay boxes onto arbitrary frames) works too: for each MQTT message, find the recorded frame whose decoded `frame_seq` equals `msg["frame_seq"]`.

## Caveats and tuning

- **Density:** with the default 1 Hz MQTT you get boxes for ~1 frame/sec of video. Lower `publishers[].mqtt.period_ms` (engine config) toward `1000/fps` for near-per-frame boxes; each message still carries the exact `frame_seq` of its frame.
- **Stamp must be enabled:** the burn-in is gated by `enable_frame_stamp` (default on) and only appears when frame output is on. The MQTT `frame_seq`/`frame_ts` fields are always present regardless.
- **Wall-clock alternative:** if you don't want to decode the grid, the human-readable ISO time and MQTT `frame_ts` let you align by wall clock — but that's approximate; `frame_seq` is the exact key.
- **Robustness:** decode from lightly-compressed frames; extreme downscaling/cropping can corrupt the cells (the checksum will catch it).

## Related

- [bounding-boxes.md](bounding-boxes.md) — the MQTT schema, the `frame_seq`/`frame_ts` fields, and the "Frame stamp" section.
- [README.md](../README.md) — outputs (MQTT, browser preview) and ports.
