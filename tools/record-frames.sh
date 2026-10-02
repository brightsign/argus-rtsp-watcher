#!/usr/bin/env bash
#
# record-frames.sh - record one Argus RTSP Watcher stream frame-by-frame into a
# folder, together with the per-frame MQTT bounding-box data, so the two can be
# joined later by frame_seq with the framebox tool.
#
# It captures both halves at once, because the boxes are NOT in the video: the
# annotated MJPEG frames only have boxes drawn on them. The structured boxes
# come from MQTT, and framebox matches an image to its MQTT message by the
# frame_seq stamp burned into each frame.
#
# Output layout (under --out, default ./recordings/<stream>-<timestamp>):
#   frames/f_00000001.jpg, f_00000002.jpg, ...   annotated frames (stamped)
#   boxes.jsonl                                   one MQTT analytics msg per line
#
# Usage:
#   ./record-frames.sh --player <ip|host> --stream <id> [--port N] \
#                      [--out DIR] [--duration SECONDS]
#
# Examples:
#   ./record-frames.sh --player 192.0.2.20 --stream lobby --port 20200
#   ./record-frames.sh --player 192.0.2.20 --stream aisle --port 20202 --duration 120
#
# Stop an open-ended recording with Ctrl-C; both ffmpeg and mosquitto_sub are
# shut down together.

set -euo pipefail

PLAYER=""
STREAM=""
PORT="20200"
OUT=""
DURATION=""

die() { echo "record-frames.sh: $*" >&2; exit 1; }

usage() {
	sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-2}"
}

while [ $# -gt 0 ]; do
	case "$1" in
		--player)   PLAYER="${2:-}"; shift 2 ;;
		--stream)   STREAM="${2:-}"; shift 2 ;;
		--port)     PORT="${2:-}"; shift 2 ;;
		--out)      OUT="${2:-}"; shift 2 ;;
		--duration) DURATION="${2:-}"; shift 2 ;;
		-h|--help)  usage 0 ;;
		*)          die "unknown argument: $1 (see --help)" ;;
	esac
done

[ -n "$PLAYER" ] || usage
[ -n "$STREAM" ] || usage
command -v ffmpeg >/dev/null 2>&1        || die "ffmpeg not found on PATH"
command -v mosquitto_sub >/dev/null 2>&1 || die "mosquitto_sub not found on PATH (install mosquitto-clients)"

if [ -z "$OUT" ]; then
	OUT="recordings/${STREAM}-$(date +%Y%m%d-%H%M%S)"
fi
FRAMES_DIR="$OUT/frames"
BOXES_FILE="$OUT/boxes.jsonl"
mkdir -p "$FRAMES_DIR"

VIDEO_URL="http://${PLAYER}:${PORT}/video"
TOPIC="bs/argus/${STREAM}/analytics"

echo "recording"
echo "  video : $VIDEO_URL"
echo "  mqtt  : ${PLAYER}:1883 topic $TOPIC"
echo "  out   : $OUT"
[ -n "$DURATION" ] && echo "  for   : ${DURATION}s" || echo "  for   : until Ctrl-C"

# Record MQTT boxes in the background, prefixing each message with a receive
# timestamp. framebox tolerates that prefix. Line-buffered so lines land promptly.
mosquitto_sub -h "$PLAYER" -t "$TOPIC" \
	| while IFS= read -r line; do printf '%s %s\n' "$(date +%s.%N)" "$line"; done \
	> "$BOXES_FILE" &
MQTT_PID=$!

cleanup() {
	kill "$MQTT_PID" 2>/dev/null || true
	wait "$MQTT_PID" 2>/dev/null || true
	echo
	echo "done: $(find "$FRAMES_DIR" -name '*.jpg' | wc -l) frames, $(wc -l < "$BOXES_FILE" 2>/dev/null || echo 0) mqtt messages in $OUT"
}
trap cleanup EXIT INT TERM

# Dump each MJPEG frame as a high-quality JPEG. -q:v 2 preserves the 10px stamp
# cells through recompression. -nostdin so Ctrl-C reaches ffmpeg cleanly.
FFMPEG_ARGS=(-hide_banner -loglevel warning -nostdin -i "$VIDEO_URL")
[ -n "$DURATION" ] && FFMPEG_ARGS+=(-t "$DURATION")
FFMPEG_ARGS+=(-q:v 2 "$FRAMES_DIR/f_%08d.jpg")

ffmpeg "${FFMPEG_ARGS[@]}"
