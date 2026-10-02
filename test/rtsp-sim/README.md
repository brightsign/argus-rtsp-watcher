# RTSP simulator (test harness)

Serve looped local video files as RTSP streams, so you can test the watcher
without live cameras. The watcher can't tell these from real RTSP feeds — point
`watcher.json` at them and everything (detection, zones, `in_zone`, preview)
works the same.

It's a [MediaMTX](https://github.com/bluenviron/mediamtx) server whose paths loop
your videos into RTSP via ffmpeg (bundled in the `:latest-ffmpeg` image).

## Quick start

1. Put video clips in `videos/` (see [layout](#video-layout)). Clips with people
   walking through your zone areas make the best tests.
2. Start it (podman preferred; docker works too):

   ```sh
   podman compose up          # or: docker compose up
   ```

3. Point `watcher.json` at this host — the paths are `lobby`, `entrance`,
   `aisle`:

   ```json
   { "id": "lobby", "rtsp_url": "rtsp://<this-host>:8554/lobby", "target_fps": 10 }
   ```

   A ready sample with all three streams and zones is in
   [`watcher.json`](watcher.json) in this folder — copy it to the player's
   `/storage/sd/watcher.json`. It points at `rtsp://rtsp-host:8554/<name>` (the
   machine running the sim); change `rtsp-host` to your sim host's name or IP if
   different, and make sure the player can resolve/reach it. Point it at the sim
   host, **not** the host serving your live cameras.

Stop with Ctrl-C (or `podman compose down`).

## Video layout

For each camera (`lobby`, `entrance`, `aisle`) pick one, first match wins:

| Put this in `videos/` | Result |
|-----------------------|--------|
| `lobby.mp4` | that single clip, looped |
| `lobby/` folder of `*.mp4` | the clips played in sorted name order (`01.mp4`, `02.mp4`, …), as a series, looped |
| `lobby.txt` (ffmpeg concat) | the listed clips as a series, looped |

Missing a video? That stream just retries every ~10 s (you can add files while
it's running). See `videos/README.md` and `videos/lobby.txt.example`.

## Adding or renaming cameras

Copy a path block in `mediamtx.yml`, change the name, and add a matching video.
Restart: `podman compose restart`.

## Plain run (no compose)

```sh
podman run --rm --name argus-rtsp-sim \
  -p 8554:8554 -p 8000:8000/udp -p 8001:8001/udp \
  -v ./mediamtx.yml:/mediamtx.yml:ro \
  -v ./stream.sh:/scripts/stream.sh:ro \
  -v ./videos:/videos:ro \
  docker.io/bluenviron/mediamtx:latest-ffmpeg
```

## How it works / notes

- `mediamtx.yml` defines one path per camera; each `runOnInit` runs `stream.sh`,
  which loops your video(s) into that RTSP path with ffmpeg. `runOnInitRestart`
  keeps it always publishing so the engine never hits a first-frame timeout.
- Streams are **re-encoded to H.264** (`-c:v libx264`, `-g 30`) for clean looping
  and quick first-frame; audio is stripped (`-an`). `-re` paces at real time.
- Ports: `8554/tcp` (RTSP), `8000/udp` + `8001/udp` (RTP/RTCP for UDP clients).
- Zones are normalized `0..1`, so test clips can be any resolution.
- For reproducibility you can pin a version, e.g. `bluenviron/mediamtx:1.9.3-ffmpeg`,
  instead of `latest-ffmpeg`.
