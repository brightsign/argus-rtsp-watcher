#!/usr/bin/env bash
set -euo pipefail

# Fetches the MediaMTX aarch64 (linux_arm64) release binary and stages it, so the
# extension can optionally run MediaMTX on the player to proxy secure (rtsps)
# cameras -- e.g. UniFi Protect -- down to plain RTSP the engine consumes.
# MediaMTX ships prebuilt static binaries, so this downloads rather than builds.
# The binary is generic aarch64 (same for every SoC), so build.sh stages the one
# output into each per-SoC dir, like the mosquitto broker and image-stream-server.
#
# Invoked by `make build-mediamtx` (and therefore `make build`). Needs curl/wget.
# Pin with MEDIAMTX_VERSION (default below); an already-present binary is reused.
#
# Usage: build-mediamtx.sh <output-dir>

OUTPUT_DIR="${1:?usage: build-mediamtx.sh <output-dir>}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MEDIAMTX_VERSION="${MEDIAMTX_VERSION:-v1.21.1}"
# MediaMTX changed its 64-bit ARM asset suffix over time (older releases use
# arm64v8, newer use arm64), so try both and take whichever exists.
ARCHES="linux_arm64 linux_arm64v8"
WORK="${REPO_ROOT}/build/mediamtx-dl"
BIN="${OUTPUT_DIR}/mediamtx"
BASE_URL="https://github.com/bluenviron/mediamtx/releases/download/${MEDIAMTX_VERSION}"

if [ -x "${BIN}" ]; then
    echo "==> mediamtx already present (${BIN})"
    exit 0
fi

fetch() {  # fetch <url> <dest> ; 0 on success
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$2" "$1" 2>/dev/null
    else
        echo "ERROR: need curl or wget to download MediaMTX" >&2
        exit 1
    fi
}

mkdir -p "${OUTPUT_DIR}" "${WORK}"
downloaded=""
for arch in ${ARCHES}; do
    tarball="mediamtx_${MEDIAMTX_VERSION}_${arch}.tar.gz"
    echo "==> Trying ${tarball}"
    if fetch "${BASE_URL}/${tarball}" "${WORK}/${tarball}" && [ -s "${WORK}/${tarball}" ]; then
        downloaded="${WORK}/${tarball}"
        break
    fi
done
if [ -z "${downloaded}" ]; then
    echo "ERROR: could not download MediaMTX ${MEDIAMTX_VERSION} (tried: ${ARCHES}) from GitHub releases" >&2
    echo "       check the version exists: https://github.com/bluenviron/mediamtx/releases" >&2
    exit 1
fi

echo "==> Extracting mediamtx binary"
tar -xzf "${downloaded}" -C "${WORK}" mediamtx
install -m 0755 "${WORK}/mediamtx" "${BIN}"

# Sanity-check the arch so a wrong download fails loudly rather than on the player.
if command -v file >/dev/null 2>&1; then
    if ! file "${BIN}" | grep -qiE "aarch64|ARM aarch64"; then
        echo "ERROR: ${BIN} is not an aarch64 binary -- wrong release asset?" >&2
        exit 1
    fi
fi

echo "==> mediamtx -> ${BIN}"
