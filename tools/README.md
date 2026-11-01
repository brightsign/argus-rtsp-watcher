# Tools

Host-side helpers for Argus RTSP Watcher: a zone picker for drawing a detection
zone, and `boxstats` for analysing recorded bounding-box data.

To relate a bounding box to a frame of a recorded video, use the `det_seq`,
`det_ts` and `source_ts` fields of each MQTT message; see
[../docs/bounding-boxes.md](../docs/bounding-boxes.md#frame-stamp-video--mqtt-sync).

## zone-picker (draw a zone)

Draw the "important area" on a frame and get its coordinates. Click-drag a
rectangle and copy either the normalized `"zone": [..]` (paste into
`config.json`) or the pixel `x0,y0,x1,y1` (for `boxstats --region`).

### Against the live camera (easiest)

`zone-picker.sh` opens the picker pre-pointed at a camera's live snapshot, so you
draw straight on the live view:

```sh
./zone-picker.sh --player xt5i --port 20201        # entrance, live-refreshing
./zone-picker.sh --player xt5i --port 20202 --once # aisle, single snapshot
```

`--port` is the stream's preview port (`20200 +` its 0-based index in
`config.json`). It serves the page over `http://localhost:8781` and opens your
browser. Needs `python3` and `curl`. Ctrl-C stops it.

### By hand / against a recorded frame

Open `zone-picker.html` directly in a browser. Either **Choose File** a recorded
frame, or type a snapshot URL (`http://<player>:<port>/image`) and **Load URL** —
tick **Live (refresh)** to keep it updating. The picker reads only image
dimensions, so a cross-origin snapshot loads without any CORS setup.

Whichever way you draw it, the `zone` only takes effect once the zone-aware
extension is deployed and you add the line to that stream in `config.json`.

## boxstats

Reports the range of person bounding-box areas for the boxes that fall inside a
region (e.g. "the fence area") across a whole recording. Use it to pick a
size/zone filter.

Its input is a JSONL file of MQTT analytics messages, one per line (a leading
receive timestamp on each line is tolerated). Capture one with:

```sh
mosquitto_sub -h <player> -t 'bs/argus/<stream-id>/analytics' > boxes.jsonl
```

```sh
make build
./bin/boxstats --region 150,60,800,560 boxes.jsonl
```

- `--region x0,y0,x1,y1` — the region in frame pixels (default `150,60,800,560`,
  the entrance fenced entry).
- `--inside base|center|full` — how a box qualifies: its bottom-center (feet,
  default), its center, or the whole box. `base` is the best proxy for where a
  person is standing.
- `--stream ID` — only count messages whose stream id contains `ID`.

It prints the count of qualifying boxes and the area min / p5 / median / mean /
p95 / max (with each as a % of the frame), plus width and height ranges.

## Note on stream ids

The MQTT topic id is the Argus stream `id` from `config.json`, which may differ
from the camera's own on-screen name. List the live topics with:

```sh
mosquitto_sub -h <player> -t 'bs/argus/#' -v
```

For the current deployment the ids are `lobby`, `entrance`, `aisle`
(the "entrance" camera is stream `entrance`).

## Build

```sh
make            # list targets
make build      # -> ./bin/boxstats
make run-tests  # unit tests
```
