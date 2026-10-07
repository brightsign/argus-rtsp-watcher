#!/usr/bin/env bash
#
# zone-picker.sh - open the Argus zone picker pointed at a player camera.
#
# Serves zone-picker.html from a local HTTP server and opens it in your browser
# pre-pointed at the camera's live snapshot, so you can draw the zone straight on
# the live view. (Serving over localhost keeps the cross-origin snapshot loading
# cleanly in every browser.)
#
# Usage:
#   ./zone-picker.sh --player <ip|host> [--port N] [--serve-port N] [--once]
#
# Examples:
#   ./zone-picker.sh --player xt5i --port 20201            # entrance, live
#   ./zone-picker.sh --player xt5i --port 20202 --once     # aisle, single snapshot
#
# --port is the stream's preview port (20200 + its 0-based index in config.json).
# --once loads a single snapshot instead of refreshing live.
# Press Ctrl-C to stop the local server.

set -euo pipefail

PLAYER=""
PORT="20201"
SERVE_PORT="8781"
LIVE=1

die() { echo "zone-picker.sh: $*" >&2; exit 1; }
usage() { sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-2}"; }

while [ $# -gt 0 ]; do
	case "$1" in
		--player)     PLAYER="${2:-}"; shift 2 ;;
		--port)       PORT="${2:-}"; shift 2 ;;
		--serve-port) SERVE_PORT="${2:-}"; shift 2 ;;
		--once)       LIVE=0; shift ;;
		-h|--help)    usage 0 ;;
		*)            die "unknown argument: $1 (see --help)" ;;
	esac
done

[ -n "$PLAYER" ] || usage
command -v python3 >/dev/null 2>&1 || die "python3 not found on PATH"
command -v curl >/dev/null 2>&1    || die "curl not found on PATH"

HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/zone-picker.html" ] || die "zone-picker.html not found next to this script"

IMG_URL="http://${PLAYER}:${PORT}/image"
curl -sf --max-time 8 -o /dev/null "$IMG_URL" \
	|| die "cannot reach $IMG_URL (is the player up and is the preview port open?)"

# Pick a free serve port starting at SERVE_PORT, so repeated runs never collide
# (a stale server on the same port is the usual cause of a broken picker URL).
SERVE_PORT="$(python3 - "$SERVE_PORT" <<'PY'
import socket, sys
start = int(sys.argv[1])
for p in range(start, start + 50):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p)); s.close(); print(p); break
    except OSError:
        s.close()
else:
    sys.exit(1)
PY
)" || die "no free serve port near $SERVE_PORT"

# Serve only a copy of the picker (not the whole tools/ tree) over localhost.
SERVE_DIR="$(mktemp -d)"
cp "$HERE/zone-picker.html" "$SERVE_DIR/"
python3 -m http.server "$SERVE_PORT" --directory "$SERVE_DIR" >/dev/null 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -rf "$SERVE_DIR"' EXIT INT TERM
sleep 0.5

ENC_IMG="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=""))' "$IMG_URL")"
PICK_URL="http://localhost:${SERVE_PORT}/zone-picker.html?url=${ENC_IMG}&live=${LIVE}"

echo "picker : $PICK_URL"
echo "camera : $IMG_URL   (live refresh: $([ "$LIVE" = 1 ] && echo on || echo off))"
echo "draw a rectangle, then copy the \"zone\": [...] line into config.json"
echo "Press Ctrl-C to stop."

if command -v xdg-open >/dev/null 2>&1; then
	xdg-open "$PICK_URL" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then
	open "$PICK_URL" || true
else
	echo "open this URL in your browser: $PICK_URL"
fi

wait "$SERVER_PID" || true
