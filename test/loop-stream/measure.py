#!/usr/bin/env python3
"""Accuracy harness: measure per-channel time-in-zone and check all channels agree.

Every watcher-test.json channel ingests the SAME looping reference stream
(rtsp://<rtsp-host>:8554/test), so all channels must report the same time-in-zone
over the same window. This subscribes to the player's MQTT broker, integrates
in-zone presence per channel over a fixed window, prints the numbers, and
PASS/FAILs on cross-channel agreement.

"Time in zone" is integrated from the per-message in_zone flags:
  person_seconds = sum over intervals of (in_zone track count) * dt
  presence_seconds = sum of dt where >=1 track is in_zone
dt comes from each message's frame_ts (capture wall-clock, ms).

Requires mosquitto_sub (no Python deps). Example:
  ./measure.py --broker 192.0.2.20 --duration 240
"""
import argparse
import json
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", required=True, help="player MQTT host (e.g. 192.0.2.20)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--prefix", default="bs/argus")
    ap.add_argument("--duration", type=float, default=240.0, help="seconds to collect (~2 loops)")
    ap.add_argument("--tolerance", type=float, default=0.10, help="max fractional spread from mean to PASS")
    args = ap.parse_args()

    topic = f"{args.prefix}/#"
    cmd = ["mosquitto_sub", "-h", args.broker, "-p", str(args.port), "-t", topic, "-v"]
    print(f"collecting {args.duration:.0f}s from {args.broker}:{args.port} topic '{topic}'", file=sys.stderr)

    # Per channel: last frame_ts (ms), accumulated person-seconds, presence-seconds,
    # message count, and a running max in-zone count (crowd peak).
    last_ts: dict[str, float] = {}
    last_cnt: dict[str, int] = {}
    person_s: dict[str, float] = {}
    presence_s: dict[str, float] = {}
    covered_s: dict[str, float] = {}   # total integrated time (sum of valid dt)
    msgs: dict[str, int] = {}
    peak: dict[str, int] = {}

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + args.duration
    try:
        assert proc.stdout is not None
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            tpc, _, payload = line.partition(" ")
            if not payload or not tpc.startswith(args.prefix + "/"):
                continue
            # bs/argus/<id>/analytics -> <id>
            parts = tpc.split("/")
            if len(parts) < 4:
                continue
            chan = parts[2]
            try:
                m = json.loads(payload)
            except json.JSONDecodeError:
                continue
            ts = m.get("frame_ts")
            if ts is None:
                continue
            ts = float(ts)
            tracks = m.get("tracks") or []
            in_zone_cnt = sum(1 for t in tracks if t.get("in_zone") is True)

            msgs[chan] = msgs.get(chan, 0) + 1
            peak[chan] = max(peak.get(chan, 0), in_zone_cnt)
            if chan in last_ts:
                dt = (ts - last_ts[chan]) / 1000.0
                if 0.0 < dt < 10.0:  # ignore clock jumps / gaps
                    person_s[chan] = person_s.get(chan, 0.0) + last_cnt[chan] * dt
                    covered_s[chan] = covered_s.get(chan, 0.0) + dt
                    if last_cnt[chan] > 0:
                        presence_s[chan] = presence_s.get(chan, 0.0) + dt
            last_ts[chan] = ts
            last_cnt[chan] = in_zone_cnt
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    chans = sorted(msgs.keys())
    if not chans:
        print("\nNO messages received. Check the broker host/port and that the "
              "watcher is running with watcher-test.json.", file=sys.stderr)
        return 2

    # Primary metric: mean people-in-zone (= time-in-zone per unit time). This is
    # sampling-robust -- it does not depend on how many snapshots a channel got,
    # unlike raw integrated person-seconds. time_in_zone over the window is just
    # mean_in_zone * duration.
    def mean_in_zone(c: str) -> float:
        cov = covered_s.get(c, 0.0)
        return (person_s.get(c, 0.0) / cov) if cov > 0 else 0.0

    print(f"\n{'channel':<10} {'msgs':>6} {'mean_in_zone':>13} {'time_in_zone_s':>15} {'peak':>5}")
    for c in chans:
        mz = mean_in_zone(c)
        print(f"{c:<10} {msgs.get(c,0):>6} {mz:>13.2f} {mz*args.duration:>15.0f} {peak.get(c,0):>5}")

    vals = [mean_in_zone(c) for c in chans]
    mean = sum(vals) / len(vals)
    lo, hi = min(vals), max(vals)
    spread = (hi - lo) / mean if mean > 0 else 0.0
    print(f"\nmean people-in-zone: mean={mean:.2f}  min={lo:.2f}  max={hi:.2f}  "
          f"spread={spread*100:.1f}%  (tolerance {args.tolerance*100:.0f}%)")
    print(f"=> time-in-zone over {args.duration:.0f}s window: ~{mean*args.duration:.0f} person-seconds/channel")

    ok_count = len(chans) == 8
    ok_spread = spread <= args.tolerance and mean > 0
    if not ok_count:
        print(f"CHANNELS: got {len(chans)}, expected 8 "
              f"({', '.join(chans)})", file=sys.stderr)
    verdict = "PASS" if (ok_spread and ok_count) else "FAIL"
    if verdict == "PASS":
        print(f"\nPASS: all {len(chans)} channels agree within "
              f"{args.tolerance*100:.0f}% (spread {spread*100:.1f}%)")
    else:
        print(f"\nFAIL: spread {spread*100:.1f}% > {args.tolerance*100:.0f}% "
              f"or channel count {len(chans)}!=8")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
