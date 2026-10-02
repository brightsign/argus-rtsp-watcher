#!/usr/bin/env bash
set -euo pipefail

# Harvests the aarch64 runtime shared libraries the engine needs from the SDK
# sysroot into a self-contained lib/ tree, so this project does not depend on a
# sibling argus install tree for them. Mirrors argus's CMake install(CODE) block:
# OpenCV + TBB + RGA + TurboJPEG + the RKNN runtime + the GStreamer runtime
# (from the SDK's ext-bundle payload) plus the few plugins the bundle omits.
#
# Invoked by `make build-libs` (and therefore `make build`); needs the SDK
# installed (scripts/fetch-sdk.sh). The libs are generic aarch64 and the same for
# every SoC, so the single output is staged into each per-SoC dir by build.sh.
#
# Usage: build-libs.sh <output-lib-dir>

OUTPUT_DIR="${1:?usage: build-libs.sh <output-lib-dir>}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ENV="${REPO_ROOT}/sdk/environment-setup-aarch64-oe-linux"
# librknnrt.so is fetched into the engine include tree by `make prep` (the engine
# links it); it is also the NPU runtime the models load at inference time.
RKNN_RT_LIB="${REPO_ROOT}/engine/include/librknnrt.so"

if [ ! -f "${SDK_ENV}" ]; then
    echo "ERROR: SDK not installed: ${SDK_ENV}" >&2
    echo "  Run scripts/fetch-sdk.sh first." >&2
    exit 1
fi

# shellcheck disable=SC1090
source "${SDK_ENV}"
SYSROOT="${OECORE_TARGET_SYSROOT:?SDK env did not set OECORE_TARGET_SYSROOT}"
SYSLIB="${SYSROOT}/usr/lib"

mkdir -p "${OUTPUT_DIR}" "${OUTPUT_DIR}/gstreamer-1.0"

# copy_glob <dir> <pattern> -- copy matching real files + dereferenced symlinks.
copy_glob() {
    local dir="$1" pattern="$2" f
    shopt -s nullglob
    for f in "${dir}"/${pattern}; do
        cp -L "${f}" "${OUTPUT_DIR}/" 2>/dev/null || true
    done
    shopt -u nullglob
}

echo "==> Harvesting runtime libraries from ${SYSLIB}"
copy_glob "${SYSLIB}" "libopencv_*.so*"
copy_glob "${SYSLIB}" "libtbb*.so*"
copy_glob "${SYSLIB}" "librga.so*"
copy_glob "${SYSLIB}" "libturbojpeg.so*"
copy_glob "${SYSLIB}" "libmosquitto.so*"

if [ -f "${RKNN_RT_LIB}" ]; then
    cp -L "${RKNN_RT_LIB}" "${OUTPUT_DIR}/"
else
    echo "ERROR: RKNN runtime not found: ${RKNN_RT_LIB}" >&2
    echo "  Run 'make prep' first." >&2
    exit 1
fi

# GStreamer runtime + plugins ship as a single payload tarball in the SDK; it
# unpacks to lib/ (versioned runtime libs) and gstreamer-1.0/ (plugin modules).
EXT_BUNDLE_TGZ="${SYSROOT}/usr/share/ext-bundle/ext-bundle-payload.tar.gz"
if [ -f "${EXT_BUNDLE_TGZ}" ]; then
    echo "==> Unpacking GStreamer runtime from ext-bundle"
    UNPACK_DIR="${REPO_ROOT}/build/_extbundle_unpack"
    rm -rf "${UNPACK_DIR}"
    mkdir -p "${UNPACK_DIR}"
    tar -xzf "${EXT_BUNDLE_TGZ}" -C "${UNPACK_DIR}"
    [ -d "${UNPACK_DIR}/lib" ] && cp -a "${UNPACK_DIR}/lib/." "${OUTPUT_DIR}/"
    [ -d "${UNPACK_DIR}/gstreamer-1.0" ] && cp -a "${UNPACK_DIR}/gstreamer-1.0/." "${OUTPUT_DIR}/gstreamer-1.0/"
    rm -rf "${UNPACK_DIR}"
else
    echo "WARNING: ext-bundle payload missing (${EXT_BUNDLE_TGZ}); GStreamer runtime not bundled" >&2
fi

# Plugins the ext-bundle omits but the pipeline requires: queue (coreelements),
# avdec_h264 (libav), qtdemux (isomp4), decodebin/uridecodebin (playback).
echo "==> Adding required GStreamer plugins"
for plugin in libgstcoreelements.so libgstlibav.so libgstisomp4.so libgstplayback.so; do
    src="${SYSLIB}/gstreamer-1.0/${plugin}"
    if [ -f "${src}" ]; then
        cp -L "${src}" "${OUTPUT_DIR}/gstreamer-1.0/"
    else
        echo "WARNING: GStreamer plugin not found in SDK: ${plugin}" >&2
    fi
done

echo "==> libs -> ${OUTPUT_DIR}"
