#!/usr/bin/env bash
set -euo pipefail

# Builds the mosquitto broker from source (cross-compiled for aarch64) and
# harvests the mosquitto client utilities + client library from the SDK sysroot,
# so this project does not depend on a sibling argus install tree for the broker.
# Mirrors argus's CMake mosquitto_broker target + install(CODE) mosquitto copy.
#
# Invoked by `make build-mosquitto` (and therefore `make build`); needs the SDK
# installed (scripts/fetch-sdk.sh). The broker is a generic aarch64 binary, so
# the single output is staged into every per-SoC dir by scripts/build.sh.
#
# Usage: build-mosquitto.sh <output-dir>

OUTPUT_DIR="${1:?usage: build-mosquitto.sh <output-dir>}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_ENV (shared cache)
MOSQUITTO_VERSION="2.0.18"
BUILD_DIR="${REPO_ROOT}/build/mosquitto-build"
TARBALL="${REPO_ROOT}/build/mosquitto-${MOSQUITTO_VERSION}.tar.gz"

if [ ! -f "${SDK_ENV}" ]; then
    echo "ERROR: SDK not installed: ${SDK_ENV}" >&2
    echo "  Run scripts/fetch-sdk.sh first." >&2
    exit 1
fi

# shellcheck disable=SC1090
source "${SDK_ENV}"
SYSROOT="${OECORE_TARGET_SYSROOT:?SDK env did not set OECORE_TARGET_SYSROOT}"

download_source() {
    if [ -f "${TARBALL}" ]; then
        return
    fi
    echo "==> Downloading mosquitto ${MOSQUITTO_VERSION}"
    mkdir -p "$(dirname "${TARBALL}")"
    curl -L -o "${TARBALL}" "https://mosquitto.org/files/source/mosquitto-${MOSQUITTO_VERSION}.tar.gz" \
        || wget -O "${TARBALL}" "https://mosquitto.org/files/source/mosquitto-${MOSQUITTO_VERSION}.tar.gz" \
        || { echo "ERROR: failed to download mosquitto (install curl or wget)" >&2; exit 1; }
}

build_broker() {
    if [ -f "${BUILD_DIR}/build/src/mosquitto" ]; then
        echo "==> mosquitto broker already built"
        return
    fi
    echo "==> Extracting mosquitto source"
    mkdir -p "${BUILD_DIR}"
    tar -xzf "${TARBALL}" -C "${BUILD_DIR}" --strip-components=1
    # The man-page subdir needs xsltproc/docbook we do not require for a broker.
    sed -i 's/add_subdirectory(man)/#add_subdirectory(man)/' "${BUILD_DIR}/CMakeLists.txt" || true

    echo "==> Cross-compiling mosquitto broker"
    mkdir -p "${BUILD_DIR}/build"
    # The OE SDK exports a ready-made CMake toolchain file when sourced; it wires
    # the cross compiler, sysroot, and flags correctly. Prefer it; fall back to
    # the sourced CC/CXX if the SDK does not provide one.
    if [ -n "${OE_CMAKE_TOOLCHAIN_FILE:-}" ] && [ -f "${OE_CMAKE_TOOLCHAIN_FILE}" ]; then
        cmake -S "${BUILD_DIR}" -B "${BUILD_DIR}/build" \
            -DCMAKE_TOOLCHAIN_FILE="${OE_CMAKE_TOOLCHAIN_FILE}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DWITH_TLS=ON -DWITH_PERSISTENCE=ON -DWITH_DOCS=OFF \
            -DWITH_TESTING=OFF -DWITH_WEBSOCKETS=OFF -DWITH_STRIP=ON
    else
        cmake -S "${BUILD_DIR}" -B "${BUILD_DIR}/build" \
            -DCMAKE_C_COMPILER="${CC%% *}" \
            -DCMAKE_SYSROOT="${SYSROOT}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DWITH_TLS=ON -DWITH_PERSISTENCE=ON -DWITH_DOCS=OFF \
            -DWITH_TESTING=OFF -DWITH_WEBSOCKETS=OFF -DWITH_STRIP=ON
    fi
    cmake --build "${BUILD_DIR}/build" -j"$(nproc)"

    if [ ! -f "${BUILD_DIR}/build/src/mosquitto" ]; then
        echo "ERROR: mosquitto broker not produced" >&2
        exit 1
    fi
}

stage_outputs() {
    echo "==> Staging mosquitto broker + client tools -> ${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"
    install -m 0755 "${BUILD_DIR}/build/src/mosquitto" "${OUTPUT_DIR}/mosquitto"

    # Client utilities and the client library ship prebuilt (aarch64) in the SDK
    # sysroot; copy them rather than rebuilding (argus does the same).
    local util
    for util in mosquitto_pub mosquitto_sub mosquitto_rr mosquitto_passwd; do
        if [ -f "${SYSROOT}/usr/bin/${util}" ]; then
            install -m 0755 "${SYSROOT}/usr/bin/${util}" "${OUTPUT_DIR}/${util}"
        fi
    done
    # libmosquitto is co-located with the binaries so they resolve it from their
    # own directory on the player.
    local lib
    for lib in "${SYSROOT}/usr/lib/libmosquitto.so" "${SYSROOT}/usr/lib/libmosquitto.so".*; do
        [ -e "${lib}" ] && cp -L "${lib}" "${OUTPUT_DIR}/" 2>/dev/null || true
    done
}

download_source
build_broker
stage_outputs
echo "==> mosquitto -> ${OUTPUT_DIR}"
