# Example watcher.json configs

Copy one of these to the player as `/storage/sd/watcher.json` (it wins the
config-discovery order, so no rebuild is needed to change cameras), edit the
URLs/zones for your site, and reboot or let the watcher hot-reload it. The full
field reference is in the main [README](../README.md#watcherjson).

Strict parsing: no comments, no extra keys. Each stream sets **exactly one**
source — `rtsp_url`, `file_path`, or `usb`.

| File | Shows |
|------|-------|
| `watcher.example.json` | The baseline — two RTSP cameras, one with a detection `zone`. This is also the default baked into the extension. |
| `watcher-usb.example.json` | A single local USB camera (`usb: true`, auto-detected device, optional capture size/fps) with a lower-half zone. |
| `watcher-advanced.example.json` | A mix: RTSP cameras behind a MediaMTX proxy, a per-stream `confidence` override, full-frame vs. partial `zone`, and a USB camera — all in one config. |

## Notes

- **`zone`** is normalized `[x0, y0, x1, y1]` (0..1). A person counts as in-zone
  when their feet (box bottom-center) fall inside it. Omit it to not report
  `in_zone`. Full frame is `[0, 0, 1, 1]`. Draw one with `tools/zone-picker.sh`.
- **`confidence`** (0..1) overrides the person-detection threshold for that
  stream; omit for the engine default (0.5).
- **MediaMTX:** BrightSign players don't yet support secure RTSP (`rtsps://` /
  SRTP), so cameras that only offer that — e.g. UniFi Protect — must be fronted
  by a plain-RTSP proxy. See [RTSP sources & MediaMTX](../README.md#rtsp-sources--mediamtx)
  in the main README. `watcher-advanced.example.json`'s `192.0.2.10:8554` URLs
  are such a proxy.
- **Testing without cameras:** serve looped video files as RTSP with the
  harnesses in [`../test/rtsp-sim/`](../test/rtsp-sim/) (multi-camera sim) or
  [`../test/loop-stream/`](../test/loop-stream/) (single clip on N channels, used
  by the accuracy test).
