#!/usr/bin/env bash
set -euo pipefail

# Fetches the Rockchip-origin headers and the RKNN runtime library the engine
# needs, from the pinned upstream repos, into engine/include/. These are NOT
# vendored in this repo: the RKNN API headers + librknnrt.so are Rockchip
# proprietary (their own headers forbid redistribution), and the sample headers
# are Apache-2.0 Rockchip example code -- so they are fetched from Rockchip at
# build time rather than committed here.
#
# Invoked by `make prep` (a prerequisite of the engine build). Pinned to the same
# RKNN version used to compile the models (scripts/build-models.sh) so runtime
# and models stay compatible. Already-present files are left untouched; run
# `make prep` after `make clean` or delete a file to re-fetch it.
#
# Usage: prep.sh            (uses RKNN_TAG, default v2.3.0)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE_DIR="${REPO_ROOT}/engine/include"
RKNN_TAG="${RKNN_TAG:-v2.3.0}"

TK="https://raw.githubusercontent.com/airockchip/rknn-toolkit2/${RKNN_TAG}"
ZOO="https://raw.githubusercontent.com/airockchip/rknn_model_zoo/${RKNN_TAG}"

# Each entry: "<destination-filename>|<url>"
FILES=(
    "rknn_api.h|${TK}/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h"
    "rknn_matmul_api.h|${TK}/rknpu2/runtime/Linux/librknn_api/include/rknn_matmul_api.h"
    "librknnrt.so|${TK}/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
    "rga_func.h|${TK}/rknpu2/examples/rknn_yolov5_demo/include/rga_func.h"
    "drm_func.h|${TK}/rknpu2/examples/rknn_yolov5_demo/include/drm_func.h"
    "rknn_box_priors.h|${ZOO}/examples/RetinaFace/cpp/rknn_box_priors.h"
    "font.h|${ZOO}/utils/font.h"
)

fetch() {
    local url="$1" dest="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "${dest}" "${url}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "${dest}" "${url}"
    else
        echo "ERROR: need curl or wget to fetch RKNN sources" >&2
        exit 1
    fi
}

mkdir -p "${INCLUDE_DIR}"
echo "==> Fetching Rockchip RKNN sources (${RKNN_TAG}) into engine/include/"
for entry in "${FILES[@]}"; do
    name="${entry%%|*}"
    url="${entry#*|}"
    dest="${INCLUDE_DIR}/${name}"
    if [ -s "${dest}" ]; then
        echo "    ${name} (present)"
        continue
    fi
    echo "    ${name}"
    tmp="${dest}.tmp.$$"
    fetch "${url}" "${tmp}"
    if [ ! -s "${tmp}" ]; then
        rm -f "${tmp}"
        echo "ERROR: fetched empty file for ${name} from ${url}" >&2
        exit 1
    fi
    mv "${tmp}" "${dest}"
done

echo "==> RKNN sources ready in ${INCLUDE_DIR}"
