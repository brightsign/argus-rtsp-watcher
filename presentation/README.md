# Argus Video Wall (on-player presentation)

A self-contained BrightSign presentation that tiles the display into a 2x2
grid and plays the four annotated MJPEG feeds the watcher exports locally on
the player. Copy these files to the root of the player's SD card and boot.

## What it does

The watcher runs one MJPEG preview server per stream at
`127.0.0.1:(20200 + stream index)`, serving the annotated `/video` feed
(boxes drawn on the frames). This presentation shows those four ports at once:

```
+----------------------+----------------------+
|        :20200        |        :20201        |
|      (stream 0)      |      (stream 1)      |
+----------------------+----------------------+
|        :20202        |        :20203        |
|      (stream 2)      |      (stream 3)      |
+----------------------+----------------------+
```

A port with no listener leaves that quadrant **solid black** and is retried
every few seconds, so feeds may start, stop, and restart without a reload.

## Live in-zone detection border

Each tile **flashes a bright red border while a person is detected inside that
stream's configured zone**. The engine fires a low-latency event the instant two
in-zone detections land within a 1-second window (keyed off the stream's `any`/`base`
zone mode), published on **MQTT-over-WebSockets** (`ws://127.0.0.1:9001`,
`bs/argus/<id>/zone_event`). The page subscribes with a tiny inline MQTT client — no
external library, since the player is offline. Each event (re)lights the tile for ~1.5s;
it clears when no further events arrive. A stream with no configured zone never flashes.

This needs the watcher's mosquitto to serve WebSockets (it adds a `listener 9001
protocol websockets`, and the broker is built `-DWITH_WEBSOCKETS=ON`). The tile→stream
mapping lives in `STREAM_IDS` at the top of `index.html` and **must match the order of
`streams[]` in `config.json`** (same order that assigns the preview ports).

## Files

| File          | Purpose                                                    |
| ------------- | ---------------------------------------------------------- |
| `autorun.brs` | Boot entry point; hosts `index.html` in a fullscreen HTML widget. |
| `index.html`  | The 2x2 video wall (HTML + CSS + JS, all inline, no deps). |

## Deploy

### SD card (first boot / no network)

1. Format an SD card as FAT32 or exFAT.
2. Copy `autorun.brs` and `index.html` to the **root** of the card (not a
   subfolder).
3. Insert the card and power on the player.

### Over the network (Local DWS)

```sh
BASE=http://<player-ip>/api/v1
AUTH="--digest -u admin:<password>"   # password defaults to the serial number

curl $AUTH -X PUT $BASE/files/sd/ -F "file=@autorun.brs"
curl $AUTH -X PUT $BASE/files/sd/ -F "file=@index.html"
curl $AUTH -X PUT $BASE/control/reboot/
```

## Configuration

- **Ports / host** - edit `HOST` and `PORTS` at the top of the `<script>` in
  `index.html`.
- **Resolution** - the wall is authored for 1080p. To retarget, change
  `SetMode("1920x1080x60p")` and the `roRectangle(0, 0, 1920, 1080)` in
  `autorun.brs` to the same resolution.
- **Fit** - tiles use `object-fit: contain` (whole frame, letterboxed). Switch
  to `cover` in `index.html` to fill and crop instead.

## Verify / debug

- **Console:** over SSH or serial the launcher prints `video wall started` and
  a `load-finished` / `load-error` line, so a black screen from a load failure
  is distinguishable from black-because-no-feeds.
- **Chrome inspector:** add an `inspector_server: { port: 2999 }` entry to the
  `config` in `autorun.brs` and set registry `html.enable_web_inspector` = `1`,
  then attach `chrome://inspect` to `<player-ip>:2999`. Remove it before
  production (the inspector buffers console output in memory).
- The ports only listen while the watcher is running with that many streams
  configured in `config.json`; ports beyond the configured stream count stay
  black by design.
