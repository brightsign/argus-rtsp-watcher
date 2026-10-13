#!/usr/bin/env bash
set -euo pipefail

# Ensures the custom BrightSign 9.1 aarch64 cross-compile SDK is available in the
# shared build cache (CACHE_DIR/sdk -- see scripts/lib/cache.sh). Resolution order
# when the SDK is not already present in the cache:
#   1. install from a brightsign-x86_64-cobra-toolchain-*.sh installer on hand
#      (arg $1 / SDK_INSTALLER, else the cache dir, repo root, or CWD); else
#   2. build it automatically via the sibling brightsign-sdk-builder provisioner,
#      which installs straight into THIS cache (needs Docker, ~25GB, hours). Opt
#      out with AUTO_BUILD_SDK=0 to get manual instructions instead.
# Because fetch-sdk is a prerequisite of the engine/mosquitto/libs builds, an
# empty-but-configured cache provisions itself on the first `make build`.
#
# "Self-contained path" matters: OE SDK installers bake absolute paths
# (SDKTARGETSYSROOT, PATH, CC/CXX --sysroot=..., and the ELF INTERP/RPATH of the
# cross-toolchain binaries themselves) into the tree at install time, relocated to
# wherever `-d <dir>` points. Because every project resolves the SAME absolute
# CACHE_DIR/sdk path, one correct install serves them all; copying in an SDK
# relocated for a different path silently breaks the build.
#
# The engine's CMake build (engine/CMakeLists.txt, `make build-engine`)
# cross-compiles for aarch64 using the toolchain + sysroots this provides:
#   sdk/environment-setup-aarch64-oe-linux   (CC/CXX/OECORE_TARGET_SYSROOT/...)
#   sdk/sysroots/aarch64-oe-linux            (OpenCV, GStreamer, RGA, mosquitto...)
#   sdk/sysroots/x86_64-oesdk-linux          (the cross toolchain)

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_DIR, SDK_ENV, CACHE_DIR
ENV_SCRIPT="${SDK_ENV}"
INSTALLER_GLOB="brightsign-x86_64-cobra-toolchain-*.sh"

# An SDK is usable only if present AND baked for THIS cache's absolute path. A
# stale-path SDK fails with "cannot execute: required file not found" when the
# cross compiler is invoked, since its ELF INTERP points elsewhere.
sdk_is_self_consistent() {
    [ -d "${SDK_DIR}/sysroots/aarch64-oe-linux" ] || return 1
    [ -f "${ENV_SCRIPT}" ] || return 1
    grep -q "SDKTARGETSYSROOT=${SDK_DIR}/sysroots/aarch64-oe-linux" "${ENV_SCRIPT}"
}

if sdk_is_self_consistent; then
    echo "SDK already installed and self-consistent at ${SDK_DIR}"
    exit 0
fi

if [ -e "${SDK_DIR}" ]; then
    echo "ERROR: ${SDK_DIR} exists but is not a self-consistent SDK for this cache" >&2
    echo "(its environment-setup-aarch64-oe-linux does not reference ${SDK_DIR})." >&2
    echo "Remove it and re-run to install fresh: rm -rf ${SDK_DIR}" >&2
    exit 1
fi

# Install from a toolchain installer if one is on hand: $1, else the shared cache
# dir, else the repo root, else CWD.
INSTALLER="${1:-}"
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${CACHE_DIR}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${REPO_ROOT}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ./${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi

if [ -n "${INSTALLER}" ] && [ -f "${INSTALLER}" ]; then
    echo "Installing SDK from ${INSTALLER} into ${SDK_DIR} (self-relocating)..."
    chmod +x "${INSTALLER}"
    # -y: non-interactive. -d: install (and relocate all baked absolute paths,
    # including the cross-toolchain binaries' ELF INTERP/RPATH) into SDK_DIR.
    "${INSTALLER}" -y -d "${SDK_DIR}"
elif [ "${AUTO_BUILD_SDK:-1}" != "1" ]; then
    # No installer, and the automatic build is disabled: fall back to guidance.
    cat >&2 <<EOF
No cross-compile SDK found in the shared cache, and no toolchain installer on hand
(automatic SDK build disabled: AUTO_BUILD_SDK=${AUTO_BUILD_SDK:-}).

Provision it once with the brightsign-sdk-builder repo, which downloads the
BrightSign OS source, builds the custom SDK, and installs it into this shared
cache (${SDK_DIR}):

    git clone https://github.com/brightsign/brightsign-sdk-builder.git
    cd brightsign-sdk-builder && make build

Or, if you already have a brightsign-x86_64-cobra-toolchain-*.sh installer:
    make fetch-sdk SDK_INSTALLER=/path/to/installer.sh
    (or drop it in the shared cache dir -- see 'make cache-info' -- or the repo root)
EOF
    exit 1
else
    # No installer on hand: build the SDK automatically. We delegate to the
    # brightsign-sdk-builder provisioner, which OWNS the multi-hour OpenEmbedded
    # build (meta-bs recipes bake in the OpenCV/GStreamer/RGA libs this engine
    # links) and installs straight into THIS cache -- so we never duplicate that
    # logic here. Locate it by override, else the conventional sibling checkout,
    # else clone it beside this repo.
    BUILDER_DIR="${SDK_BUILDER_DIR:-$(cd "${REPO_ROOT}/.." && pwd)/brightsign-sdk-builder}"
    if [ ! -f "${BUILDER_DIR}/Makefile" ]; then
        echo "==> brightsign-sdk-builder not found at ${BUILDER_DIR}; cloning it"
        git clone https://github.com/brightsign/brightsign-sdk-builder.git "${BUILDER_DIR}"
    fi
    [ -f "${BUILDER_DIR}/Makefile" ] || {
        echo "ERROR: brightsign-sdk-builder is unavailable at ${BUILDER_DIR}" >&2
        exit 1
    }

    cat <<EOF
==> No SDK in the shared cache and no installer on hand.
    Building it now via brightsign-sdk-builder:
        ${BUILDER_DIR}
    This is a one-time OpenEmbedded build: it needs Docker, ~25GB of disk, and
    network, and TAKES HOURS. It installs straight into ${SDK_DIR} and is reused
    by every project on this box. Set AUTO_BUILD_SDK=0 to skip and provision
    manually instead.
EOF
    # The builder resolves its cache via ARGUS_CACHE_DIR; hand it THIS repo's
    # already-resolved absolute cache path so both agree on one physical cache.
    ARGUS_CACHE_DIR="${CACHE_DIR}" "${MAKE:-make}" -C "${BUILDER_DIR}" sdk
fi

if ! sdk_is_self_consistent; then
    echo "ERROR: SDK provisioned but environment-setup-aarch64-oe-linux does not" >&2
    echo "reference ${SDK_DIR} -- relocation did not take effect as expected." >&2
    exit 1
fi

# The RKNN runtime library ships separately from the BrightSign SDK installer.
# Pinned to the same RKNN version as `make prep` and model compilation so the
# runtime, API headers, and compiled models stay compatible. The builder path
# already installs this; the check below is a harmless idempotent safety net for
# the installer path.
RKNN_TAG="${RKNN_TAG:-v2.3.0}"
RKNN_LIB="${SDK_DIR}/sysroots/aarch64-oe-linux/usr/lib/librknnrt.so"
if [ ! -f "${RKNN_LIB}" ]; then
    echo "Fetching librknnrt.so (Rockchip NPU runtime, ${RKNN_TAG})..."
    wget -O "${RKNN_LIB}" \
        "https://github.com/airockchip/rknn-toolkit2/raw/${RKNN_TAG}/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
fi

echo "SDK installed and self-consistent at ${SDK_DIR}"
echo "Source it before building: source ${SDK_DIR}/environment-setup-aarch64-oe-linux"
echo "Or just run: make build-engine"
