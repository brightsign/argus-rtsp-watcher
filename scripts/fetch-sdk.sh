#!/usr/bin/env bash
set -euo pipefail

# Installs a self-contained BrightSign 9.0 aarch64 cross-compile SDK into
# ./sdk (gitignored). "Self-contained" matters here: OE SDK installers bake
# absolute paths (SDKTARGETSYSROOT, PATH, CC/CXX --sysroot=..., and even the
# ELF INTERP/RPATH of the cross-toolchain binaries themselves) into the
# extracted tree at install time, relocated to wherever `-d <dir>` points.
# Symlinking in an SDK that was installed for a *different* absolute path
# (e.g. another repo's ./sdk) silently reintroduces those stale paths and
# breaks the build for everyone except whoever happened to have that other
# path on disk. Always install fresh with `-d` pointed at THIS repo's ./sdk.
#
# The engine's CMake build (engine/CMakeLists.txt, invoked by `make build-engine`)
# cross-compiles for aarch64 using the toolchain and sysroots this script installs.
# It needs:
#   sdk/environment-setup-aarch64-oe-linux   (sets CC/CXX/OECORE_TARGET_SYSROOT/etc.)
#   sdk/sysroots/aarch64-oe-linux            (target headers/libs: OpenCV, GStreamer, RGA, mosquitto...)
#   sdk/sysroots/x86_64-oesdk-linux          (the aarch64-oe-linux-{gcc,g++,...} cross toolchain)
#
# This SDK is large (~1.6G extracted) and is not committed to this repo.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${REPO_ROOT}/sdk"
ENV_SCRIPT="${SDK_DIR}/environment-setup-aarch64-oe-linux"
INSTALLER_GLOB="brightsign-x86_64-cobra-toolchain-*.sh"

# An SDK is only usable here if it is both present AND baked for THIS repo's
# ./sdk path (not relocated for some other checkout). A stale-path SDK fails
# with "command not found" or "cannot execute: required file not found" when
# the cross compiler is invoked, since its ELF INTERP points elsewhere.
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
    echo "ERROR: ${SDK_DIR} exists but is not a self-consistent SDK for this repo" >&2
    echo "(its environment-setup-aarch64-oe-linux does not reference ${SDK_DIR})." >&2
    echo "Remove it and re-run this script to install fresh: rm -rf ${SDK_DIR}" >&2
    exit 1
fi

# Prefer an installer passed as $1, else look for one in the repo root or CWD.
INSTALLER="${1:-}"
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${REPO_ROOT}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ./${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi

if [ -z "${INSTALLER}" ] || [ ! -f "${INSTALLER}" ]; then
    cat <<'EOF'
Could not find a BrightSign aarch64 SDK installer.

This script needs brightsign-x86_64-cobra-toolchain-<version>.sh, the BrightSign
9.0 SDK installer built by the argus-audience-measurement-extension repo's
scripts/runall.sh (step0_setup), which:
  1. downloads the BrightSign OS 9.0 source tarballs,
  2. builds them in a container (bsoe-build) with `MACHINE=cobra ./bsbb brightsign-sdk`,
  3. produces brightsign-x86_64-cobra-toolchain-<version>.sh under
     brightsign-oe/build/tmp-glibc/deploy/sdk/.

If you already have that installer, do one of:
  - Run this script again with the installer path as an argument:
      scripts/fetch-sdk.sh /path/to/brightsign-x86_64-cobra-toolchain-9.0.189.sh
  - Place the installer .sh file at the repo root and re-run this script.

Do NOT symlink or copy in an already-extracted sdk/ directory from another
checkout (e.g. argus-audience-measurement-extension/sdk) -- that SDK's
absolute paths were relocated for THAT repo's location, not this one, and
will fail (or silently depend on that other checkout still existing at its
exact path). Always run the installer with -d pointed at this repo's ./sdk
so the OE relocation step bakes the right path in.

See ../argus-audience-measurement-extension/scripts/runall.sh (step0_setup) for
how the installer itself is produced.
EOF
    exit 1
fi

echo "Installing SDK from ${INSTALLER} into ${SDK_DIR} (self-relocating)..."
chmod +x "${INSTALLER}"
# -y: non-interactive. -d: install (and relocate all baked absolute paths,
# including the cross-toolchain binaries' ELF INTERP/RPATH) into SDK_DIR.
"${INSTALLER}" -y -d "${SDK_DIR}"

if ! sdk_is_self_consistent; then
    echo "ERROR: SDK installed but environment-setup-aarch64-oe-linux does not" >&2
    echo "reference ${SDK_DIR} -- relocation did not take effect as expected." >&2
    exit 1
fi

# The RKNN runtime library ships separately from the BrightSign SDK installer.
# Pinned to the same RKNN version as `make prep` and model compilation so the
# runtime, API headers, and compiled models stay compatible.
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
