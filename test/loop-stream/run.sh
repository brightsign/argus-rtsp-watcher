#!/usr/bin/env bash
# Start (or restart) the looping reference RTSP stream on rtsp-host.
# Serves rtsp://<rtsp-host>:8554/test -- one path, looped forever, shared by all
# watcher-test.json channels. Stop with: podman stop argus-loop-stream
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
name="argus-loop-stream"
image="docker.io/bluenviron/mediamtx:latest-ffmpeg"

if [ ! -f "${here}/video/people-walking.mp4" ]; then
  echo "missing ${here}/video/people-walking.mp4 (transcode the clip first)" >&2
  exit 1
fi

podman rm -f "${name}" >/dev/null 2>&1 || true

podman run --rm -d --name "${name}" --network host \
  -v "${here}/mediamtx.yml:/mediamtx.yml:ro,Z" \
  -v "${here}/video:/video:ro,Z" \
  "${image}"

echo "started ${name} -> rtsp://$(hostname):8554/test"
echo "verify: ffprobe -rtsp_transport tcp rtsp://$(hostname):8554/test"
