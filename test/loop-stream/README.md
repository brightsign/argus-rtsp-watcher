# 8-stream accuracy test

> New to this? **[TESTING-GUIDE.md](TESTING-GUIDE.md)** covers how to find a
> suitable clip, establish ground truth, and a copy-paste prompt to have an
> agent run the whole test.


Loops one reference clip as a single RTSP stream and has all 8 watcher channels
ingest it, then checks every channel reports the same time-in-zone. Because all
channels see identical frames, cross-channel agreement proves the multi-stream
pipeline is sound; the absolute number is compared against the externally
measured ground truth in `../test-video/people-walking-analysis.md`.

## Files

- `video/people-walking.mp4` — reference clip, H.264 1280x720 (transcoded from the
  VP9 original in `../test-video/`).
- `mediamtx.yml` + `run.sh` — serve `rtsp://<rtsp-host>:8554/test`, looped forever.
- `watcher-test.json` — 8 channels (`test1`..`test8`), all pointing at that one
  stream, zone = full frame `[0,0,1,1]` (so time-in-zone = total person-seconds).
- `measure.py` — subscribe to the player's MQTT, integrate per-channel
  time-in-zone over a window, PASS/FAIL on cross-channel agreement.

## Run it

```sh
# 1. Start the reference stream on rtsp-host
./run.sh
ffprobe -rtsp_transport tcp rtsp://192.0.2.10:8554/test   # verify (H.264 1280x720)

# 2. Point the player at the test config (watcher hot-reloads it; no reboot)
sshpass -p none scp -o PreferredAuthentications=password -o PubkeyAuthentication=no \
    watcher-test.json brightsign@xt5i:/storage/sd/watcher.json

# 3. Confirm all 8 channels publish
mosquitto_sub -h 192.0.2.20 -t 'bs/argus/#' -C 16 -v

# 4. Measure + compare (~3-4 loops; longer = less sampling noise)
./measure.py --broker 192.0.2.20 --duration 420 --tolerance 0.15
```

## Notes

- The reference stream is shared by all 8 channels, so they see identical frames.
- Under 8-stream load the player's MQTT publish is sparse (~1 msg/8s per channel),
  so `measure.py` compares **mean people-in-zone** (time-in-zone per unit time),
  which is independent of how many snapshots a channel got; sample longer to
  tighten the spread.
- Stop the stream: `podman stop argus-loop-stream`.
- Restore the player's normal config afterward (copy your real watcher.json back
  to `/storage/sd/watcher.json`).
