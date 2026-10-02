#!/bin/sh
# Publish video(s) for one camera to MediaMTX as a looping RTSP stream.
# Called by mediamtx.yml runOnInit as: sh /scripts/stream.sh <name>
#
# Input selection for <name>, first match wins:
#   1. a folder of clips:  /videos/<name>/*.mp4  -> played in sorted order, as a series
#   2. an explicit playlist: /videos/<name>.txt  -> ffmpeg concat format
#   3. a single file:        /videos/<name>.mp4
# The whole thing loops forever. Missing input -> sleep + exit so
# runOnInitRestart retries (you can add videos while it's running).

set -u

name="${1:?usage: stream.sh <camera-name>}"
videos="/videos"
port="${RTSP_PORT:-8554}"
target="rtsp://localhost:${port}/${name}"
list="/tmp/${name}.concat.txt"

if [ -d "${videos}/${name}" ] && ls "${videos}/${name}"/*.mp4 >/dev/null 2>&1; then
    : > "$list"
    for f in "${videos}/${name}"/*.mp4; do
        echo "file '${f}'" >> "$list"
    done
    set -- -f concat -safe 0 -i "$list"
elif [ -f "${videos}/${name}.txt" ]; then
    set -- -f concat -safe 0 -i "${videos}/${name}.txt"
elif [ -f "${videos}/${name}.mp4" ]; then
    set -- -i "${videos}/${name}.mp4"
else
    echo "rtsp-sim: no video for '${name}' (expected ${videos}/${name}.mp4, ${videos}/${name}.txt, or ${videos}/${name}/*.mp4)" >&2
    sleep 10   # avoid a hot restart loop while you add files
    exit 1
fi

echo "rtsp-sim: publishing '${name}' -> ${target}" >&2
exec ffmpeg -hide_banner -loglevel warning \
    -re -stream_loop -1 "$@" \
    -c:v libx264 -preset veryfast -tune zerolatency -g 30 -pix_fmt yuv420p \
    -an -f rtsp -rtsp_transport tcp "$target"
