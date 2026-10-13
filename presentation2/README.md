# Argus Video Wall - 8-up (on-player presentation)

A self-contained BrightSign presentation that tiles the display into a 4x2
grid and plays up to eight annotated MJPEG feeds the watcher exports locally
on the player. Copy these files to the root of the player's SD card and boot.

This is the eight-pane variant of [`../presentation`](../presentation) (four
panes). The two are separate SD-card images - a card has one `autorun.brs`, so
pick one presentation per card.

## What it does

The watcher runs one MJPEG preview server per stream at
`127.0.0.1:(20200 + stream index)`, serving the annotated `/video` feed
(boxes drawn on the frames). This presentation shows up to eight ports at once:

```
+----------+----------+----------+----------+
|  :20200  |  :20201  |  :20202  |  :20203  |
+----------+----------+----------+----------+
|  :20204  |  :20205  |  :20206  |  :20207  |
+----------+----------+----------+----------+
```

A port with no listener leaves that pane **solid black** and is retried every
few seconds, so feeds may start, stop, and restart without a reload. With
fewer than eight streams configured, the unused panes simply stay black.

## Live in-zone detection border and audio alert

When a person is detected inside a stream's configured zone, a **red border is burned
into the video itself** by the engine — fired immediately, held while the zone stays
triggered, and cleared when the trigger condition clears. Its width matches that
stream's `zone_border_thickness`. This wall just displays the feeds, so the border
shows up automatically; there is no browser-side visual flash. A pane with no
configured zone (or no stream) never shows the border.

The page also plays a short **audio alert tone** on each in-zone event, via a tiny
subscribe-only MQTT-over-WebSockets client (inline, no library) that listens for the
engine's `bs/argus/<id>/zone_event` messages and sounds a Web Audio beep, rate-limited
to one tone every two seconds across all streams. This needs the watcher's mosquitto to
serve WebSockets (`listener 9001 protocol websockets`, broker built
`-DWITH_WEBSOCKETS=ON`). It subscribes to `bs/argus/#`, so no tile→stream mapping is
required. The player must allow HTML audio autoplay, which BrightSign's HTML engine does
by default.

## Files

| File          | Purpose                                                    |
| ------------- | ---------------------------------------------------------- |
| `autorun.brs` | Boot entry point; hosts `index.html` in a fullscreen HTML widget. |
| `index.html`  | The 4x2 video wall (HTML + CSS + JS, all inline, no deps). |

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

- **Pane count / ports / host** - edit `HOST` and `PORTS` at the top of the
  `<script>` in `index.html`. To change the pane count, add or remove
  `<div class="tile">` entries to match the length of `PORTS`.
- **Grid shape** - the CSS uses `repeat(4, 1fr)` columns x `repeat(2, 1fr)`
  rows. Change those two values in `index.html` to re-tile (e.g. `repeat(2,..)`
  by `repeat(4,..)` for a 2-wide x 4-tall layout).
- **Resolution** - the wall is authored for 1080p. To retarget, change
  `SetMode("1920x1080x60p")` and the `roRectangle(0, 0, 1920, 1080)` in
  `autorun.brs` to the same resolution.
- **Fit** - tiles use `object-fit: contain` (whole frame, letterboxed). Switch
  to `cover` in `index.html` to fill and crop instead. Note: with eight 16:9
  feeds in tall 4x2 panes, `contain` leaves top/bottom black bars; that is
  expected for the pane shape.

## Verify / debug

- **Console:** over SSH or serial the launcher prints
  `argus 8-up video wall started` and a `load-finished` / `load-error` line,
  so a black screen from a load failure is distinguishable from
  black-because-no-feeds.
- **Chrome inspector:** add an `inspector_server: { port: 2999 }` entry to the
  `config` in `autorun.brs` and set registry `html.enable_web_inspector` = `1`,
  then attach `chrome://inspect` to `<player-ip>:2999`. Remove it before
  production (the inspector buffers console output in memory).
- The ports only listen while the watcher is running with that many streams
  configured in `config.json`; ports beyond the configured stream count stay
  black by design.
