# Accuracy testing guide — finding a clip and running the test

This harness loops one reference clip as a single RTSP stream and has all 8
watcher channels ingest it, then checks every channel reports the same
time-in-zone (see [README.md](README.md) for the mechanics). The reference clip
is **not** committed (it's large and often not redistributable) — you supply
your own. This guide covers how to pick one, how to establish ground truth, and
a copy-paste prompt to have an agent run the whole thing.

## 1. Find a suitable clip

What makes a good clip for this test:

- **People in it.** The pipeline detects people (and gaze). Pedestrian/retail/
  crowd footage works well; the busier the scene, the larger and more stable the
  time-in-zone number.
- **A fixed camera** (ideally). A single static viewpoint makes a fixed `zone`
  meaningful. Clips that cut between angles (like the original `people-walking`
  clip, which is 8 shots spliced together) still work for the cross-channel
  *consistency* check, but a drawn zone then frames a different area per shot.
- **1–3 minutes**, so each loop is a meaningful measurement window and the loop
  seam is infrequent.
- **Any codec is fine** — you transcode to H.264 720p below (RTSP needs H.264/
  H.265; the engine downsamples to 320×320 for detection, so 720p is plenty).

Where to get one, license permitting:

- Your own camera footage (best — matches your real deployment).
- Free stock clips (e.g. Pexels, Pixabay, Coverr) — search "people walking
  street", "pedestrians", "mall crowd". Check the license allows your use.

Transcode it to what the harness expects:

```sh
ffmpeg -i <your-clip> -vf scale=1280:720 -r 30 -c:v libx264 \
  -preset veryfast -crf 23 -pix_fmt yuv420p -an \
  test/loop-stream/video/people-walking.mp4
```

(The harness streams `test/loop-stream/video/people-walking.mp4`. That path is
gitignored — never commit the media.)

## 2. Establish ground truth (optional but recommended)

The cross-channel check (all 8 agree) is valid regardless of the absolute
number, because every channel sees identical frames. To also sanity-check the
*magnitude*, measure the clip independently:

- Run an offline person detector + tracker on the clip (every Nth frame),
  summing each track's seconds inside your chosen zone → expected person-seconds
  per loop. An example writeup for the original clip is in
  [`../test-video/people-walking-analysis.md`](../test-video/people-walking-analysis.md).
- Or, for a full-frame zone, "time in zone" = total person-seconds on screen;
  eyeball it from a manual count × average dwell.

Different detectors won't match exactly (the on-device YOLOX is more constrained
than a desktop model), so compare order-of-magnitude, not to the decimal.

## 3. Run the test

See [README.md](README.md) for the four commands (start stream → copy
`watcher-test.json` to the player → confirm 8 channels → `measure.py`). A longer
`--duration` averages out the sparse MQTT sampling and tightens the spread.

## 4. Have an agent run it

Copy the prompt below to an agent (e.g. Claude Code) that has shell access to the
streaming host and SSH access to the player. Fill in the three placeholders.

```text
Run the argus-rtsp-watcher 8-stream accuracy test in test/loop-stream/.

Inputs:
- STREAM_HOST_IP = <ip the player will reach this host at>   e.g. 192.0.2.10
- PLAYER         = <player ssh host>                         e.g. xt5i  (user brightsign)
- CLIP           = <path to a video of people; any codec>

Do this and report a PASS/FAIL with the per-channel table:
1. Transcode CLIP to H.264 720p at test/loop-stream/video/people-walking.mp4:
   ffmpeg -i <CLIP> -vf scale=1280:720 -r 30 -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p -an test/loop-stream/video/people-walking.mp4
2. Start the loop stream:  test/loop-stream/run.sh
   Verify:  ffprobe -rtsp_transport tcp rtsp://STREAM_HOST_IP:8554/test   (expect H.264 1280x720)
3. Make sure test/loop-stream/watcher-test.json has all 8 channels' rtsp_url set to
   rtsp://STREAM_HOST_IP:8554/test , then copy it to the player (watcher hot-reloads it):
   scp test/loop-stream/watcher-test.json brightsign@PLAYER:/storage/sd/config.json
4. Confirm all 8 channels publish:
   mosquitto_sub -h PLAYER -t 'bs/argus/#' -C 16 -v    (expect test1..test8)
5. Measure and compare (run long; longer = tighter):
   test/loop-stream/measure.py --broker PLAYER --duration 1200 --tolerance 0.15
6. Report the per-channel mean_in_zone / time_in_zone table and the PASS/FAIL.
   PASS = all 8 channels within tolerance of the mean (they ingest identical frames,
   so they must agree). If a channel is missing or zero, check that stream's engine
   log on the player (/tmp/<stream-id>.log, and /tmp/main.log for the watcher) and that the RTSP URL is reachable from the player.

Guardrails:
- Do not commit any video/media (they're gitignored; keep them out of git).
- The player SSH is a REPL, not a shell; use scp for file copy, and the watcher
  hot-reloads /storage/sd/config.json (no reboot needed to change the config).
- Stop the stream when done:  podman stop argus-loop-stream
```
