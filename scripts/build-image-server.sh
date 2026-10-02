#!/usr/bin/env bash
set -euo pipefail

# Builds the image-stream-server (the per-stream MJPEG preview server) from its
# own upstream repo, so this project does not depend on a sibling argus install
# tree to supply the binary. Invoked by `make build-image-server` (and therefore
# `make build`); never run bare `go build` here -- the Makefile is the single
# source of truth.
#
# The output is a single statically-linked aarch64 Go binary. It is
# SoC-independent (CGO disabled, pure arm64), so one build is staged into every
# per-SoC directory by scripts/build.sh -- there is no per-SoC variant.
#
# Usage: build-image-server.sh <output-binary-path>
#   IMAGE_SERVER_REF   git ref to build (default: main)
#   FORCE_UPDATE=1     re-pull an existing clone before building

OUTPUT_BIN="${1:?usage: build-image-server.sh <output-binary-path>}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLONE_DIR="${REPO_ROOT}/build/bs-image-stream-server"
UPSTREAM_URL="https://github.com/brightsign/bs-image-stream-server.git"
IMAGE_SERVER_REF="${IMAGE_SERVER_REF:-main}"

# Upstream's own build target and output path (see its Makefile: build-arm64).
UPSTREAM_BIN="${CLONE_DIR}/bin/image-stream-server-arm64"

# The index.html we bake into the binary's embedded static assets. Keeping it in
# this repo (not relying on upstream's default) pins the preview UI regardless of
# upstream changes.
INDEX_PATCH="${REPO_ROOT}/scripts/patches/image-stream-server-index.html"
INDEX_DST="${CLONE_DIR}/internal/server/static/index.html"

# Commit-tracking lets repeat builds skip the Go compile when nothing changed.
COMMIT_FILE="${REPO_ROOT}/build/.image-stream-server-commit"

clone_or_update() {
    if [ ! -d "${CLONE_DIR}" ]; then
        echo "==> Cloning bs-image-stream-server (${IMAGE_SERVER_REF})"
        git clone --depth=1 --branch "${IMAGE_SERVER_REF}" "${UPSTREAM_URL}" "${CLONE_DIR}"
    elif [ "${FORCE_UPDATE:-0}" = "1" ]; then
        echo "==> FORCE_UPDATE set, pulling latest ${IMAGE_SERVER_REF}"
        git -C "${CLONE_DIR}" fetch --depth=1 origin "${IMAGE_SERVER_REF}"
        git -C "${CLONE_DIR}" reset --hard FETCH_HEAD
    else
        echo "==> bs-image-stream-server already cloned (set FORCE_UPDATE=1 to re-pull)"
    fi
}

apply_patch() {
    if [ ! -f "${INDEX_PATCH}" ]; then
        echo "ERROR: missing preview UI patch: ${INDEX_PATCH}" >&2
        exit 1
    fi
    if [ ! -f "${INDEX_DST}" ]; then
        echo "ERROR: upstream static index.html not at expected path: ${INDEX_DST}" >&2
        echo "  (upstream layout changed; update INDEX_DST in $(basename "${BASH_SOURCE[0]}"))" >&2
        exit 1
    fi
    echo "==> Applying preview UI patch"
    cp "${INDEX_PATCH}" "${INDEX_DST}"
}

# The embedded index.html is part of the binary, so a patch change must force a
# rebuild even when the upstream commit is unchanged -- fold the patch hash into
# the tracked identity.
current_identity() {
    local commit patch_hash
    commit="$(git -C "${CLONE_DIR}" rev-parse HEAD 2>/dev/null || echo "")"
    patch_hash="$(sha256sum "${INDEX_PATCH}" | awk '{print $1}')"
    echo "${commit}:${patch_hash}"
}

build() {
    local go_bin goversion_full goversion_short
    go_bin="$(command -v go)"
    goversion_full="$("${go_bin}" version | awk '{print $3}' | sed 's/^go//')"
    goversion_short="$(echo "${goversion_full}" | awk -F. '{print $1"."$2}')"

    # Upstream may pin a newer toolchain than the host Go. Let Go fetch it on
    # demand rather than failing the build; GOTOOLCHAIN=auto is the documented
    # mechanism for that.
    export GOTOOLCHAIN=auto
    ( cd "${CLONE_DIR}" && make build-arm64 )

    if [ ! -f "${UPSTREAM_BIN}" ]; then
        echo "ERROR: build produced no binary at ${UPSTREAM_BIN}" >&2
        exit 1
    fi
    mkdir -p "$(dirname "${OUTPUT_BIN}")"
    install -m 0755 "${UPSTREAM_BIN}" "${OUTPUT_BIN}"
    echo "==> image-stream-server -> ${OUTPUT_BIN} (go ${goversion_short})"
}

main() {
    clone_or_update
    apply_patch

    local identity
    identity="$(current_identity)"
    if [ -f "${OUTPUT_BIN}" ] && [ -f "${COMMIT_FILE}" ] && [ "$(cat "${COMMIT_FILE}")" = "${identity}" ]; then
        echo "==> image-stream-server unchanged, skipping build"
        return 0
    fi

    build
    echo "${identity}" > "${COMMIT_FILE}"
}

main "$@"
