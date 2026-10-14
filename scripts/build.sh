#!/usr/bin/env bash
set -euo pipefail

# Assembles the multi-SoC staging tree and packages the extension. Invoked by
# `make package` (and therefore `make build`) after build-watcher/build-engine
# have produced the watcher and engine binaries; never run this directly with
# `go build` or bare cmake -- the Makefile is the single source of truth for
# how those binaries get built.
#
# Layout produced under $STAGING_DIR (mirrors argus's own staging layout):
#   bsext_init                  <- extension root; detects SoC, execs ${SOC}/watcher
#   <SOC>/watcher                arm64 Go supervisor (same binary in every SoC dir)
#   <SOC>/argus-engine           aarch64 engine (same binary in every SoC dir --
#                                 TARGET_SOC only changes the cmake install path,
#                                 not compiled code; see engine/CMakeLists.txt)
#   <SOC>/config.json            combined operator config: streams + mediamtx +
#                                 engine template (packaged default; SD override wins)
#   <SOC>/image-stream-server     MJPEG preview server (built by build-image-server;
#                                 one SoC-independent arm64 binary, staged into each SoC dir)
#   <SOC>/mediamtx                optional RTSPS proxy (build-mediamtx; SoC-independent)
#   <SOC>/bin/                   mosquitto broker + client utilities (build-mosquitto;
#                                 SoC-independent, same in every SoC dir)
#   <SOC>/lib/                   OpenCV/GStreamer/RKNN/RGA runtime libs (build-libs;
#                                 SoC-independent, same in every SoC dir)
#   <SOC>/model/                 RetinaFace.rknn, yolox_s.rknn, coco labels
#                                 (build-models; SoC-specific, compiled per SoC)
#
# cmd/watcher/main.go resolves every one of those per-SoC paths relative to
# the watcher binary's own directory (os.Executable()), so each <SOC> dir
# must be self-contained and flat -- nothing the watcher needs may live only
# at the staging root.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

SOC_LIST="${SOC_LIST:-RK3588 RK3576 RK3568}"
STAGING_DIR="${STAGING_DIR:-staging}"
WATCHER_BIN="${WATCHER_BIN:-${REPO_ROOT}/build/watcher}"
ENGINE_BIN="${ENGINE_BIN:-${REPO_ROOT}/build/rk3588/argus-engine}"
IMAGE_SERVER_BIN="${IMAGE_SERVER_BIN:-${REPO_ROOT}/build/image-stream-server}"
CONFIG_SERVER_BIN="${CONFIG_SERVER_BIN:-${REPO_ROOT}/build/config-server}"
# Runtime assets produced on demand (no sibling argus tree): mosquitto broker +
# client tools, the harvested runtime lib/ tree, and the per-SoC compiled models.
MOSQUITTO_DIR="${MOSQUITTO_DIR:-${REPO_ROOT}/build/mosquitto}"
LIBS_DIR="${LIBS_DIR:-${REPO_ROOT}/build/libs}"
MODELS_DIR="${MODELS_DIR:-${REPO_ROOT}/build/models}"
# Optional on-player RTSPS proxy (build-mediamtx); SoC-independent aarch64 binary.
MEDIAMTX_DIR="${MEDIAMTX_DIR:-${REPO_ROOT}/build/mediamtx}"
EXT_NAME="${EXT_NAME:-npu_watch}"
BUILD_TS="${BUILD_TS:-$(date +%s)}"
DEV_ZIP="${DEV_ZIP:-argus-rtsp-watcher-${BUILD_TS}.zip}"
EXT_ZIP="${EXT_ZIP:-argus-rtsp-watcher-ext-${BUILD_TS}.zip}"

# --- output styling (matches the argus packager look) ---
if [ -t 1 ]; then
    C_BLUE=$'\033[0;34m'; C_GREEN=$'\033[0;32m'; C_YELLOW=$'\033[1;33m'; C_RED=$'\033[0;31m'; C_RESET=$'\033[0m'
else
    C_BLUE=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_RESET=''
fi
log()  { printf '%s[PACKAGE]%s %s\n' "${C_BLUE}" "${C_RESET}" "$*"; }
ok()   { printf '%s✅ %s%s\n' "${C_GREEN}" "$*" "${C_RESET}"; }
warn() { printf '%s⚠️  %s%s\n' "${C_YELLOW}" "$*" "${C_RESET}"; }
die()  { printf '%s❌ %s%s\n' "${C_RED}" "$*" "${C_RESET}" >&2; exit 1; }

# Friendly BrightSign model name for a Rockchip SoC.
soc_label() {
    case "$1" in
        RK3588) echo "XT5" ;;
        RK3568) echo "LS5" ;;
        RK3576) echo "XS6/XD6" ;;
        *) echo "$1" ;;
    esac
}

human_size() { du -h "$1" 2>/dev/null | cut -f1; }

START_TS="$(date +%s)"

require_file() {
    local path="$1" what="$2" hint="${3:-}"
    if [ ! -f "${path}" ]; then
        [ -n "${hint}" ] && die "missing ${what}: ${path}"$'\n  '"${hint}"
        die "missing ${what}: ${path}"
    fi
}

require_dir() {
    local path="$1" what="$2" hint="${3:-}"
    if [ ! -d "${path}" ]; then
        [ -n "${hint}" ] && die "missing ${what}: ${path}"$'\n  '"${hint}"
        die "missing ${what}: ${path}"
    fi
}

echo "BrightSign Argus RTSP Watcher - Package Creation"
echo "================================================"
echo
echo "Configuration:"
echo "  SOCs:      ${SOC_LIST}"
echo "  Extension: ${EXT_NAME}"
echo

log "Checking prerequisites..."
require_file "${WATCHER_BIN}" "watcher binary (run 'make build-watcher')"
require_file "${ENGINE_BIN}" "engine binary (run 'make build-engine')"
require_file "${IMAGE_SERVER_BIN}" "image-stream-server binary (run 'make build-image-server')"
require_file "${CONFIG_SERVER_BIN}" "config-server binary (run 'make build-config-server')"
require_file "${MOSQUITTO_DIR}/mosquitto" "mosquitto broker (run 'make build-mosquitto')"
require_file "${MEDIAMTX_DIR}/mediamtx" "mediamtx binary (run 'make build-mediamtx')"
require_dir "${LIBS_DIR}" "harvested runtime libs (run 'make build-libs')"
require_file "packaging/bsext_init" "bsext_init"
require_file "packaging/make-extension-lvm" "make-extension-lvm"
require_file "configs/config.json" "combined operator config (streams + mediamtx + engine)"
require_file "configs/mediamtx.yml" "default mediamtx config"
command -v mksquashfs >/dev/null 2>&1 || die "mksquashfs not found; install squashfs-tools to build the LVM extension package"
ok "Prerequisites check passed"

log "Creating package structure..."
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"
install -m 0755 packaging/bsext_init "${STAGING_DIR}/bsext_init"
ok "Package structure created"

for soc in ${SOC_LIST}; do
    label="$(soc_label "${soc}")"
    log "Staging ${soc} (${label})..."
    soc_models="${MODELS_DIR}/${soc}"
    require_file "${soc_models}/RetinaFace.rknn" "RetinaFace.rknn for ${soc} (run 'make build-models')"
    require_file "${soc_models}/yolox_s.rknn" "yolox_s.rknn for ${soc} (run 'make build-models')"
    # Required, not optional: engine/src/postprocess.cc hardcodes
    # "model/coco_80_labels_list.txt" for YOLOX class names. Staging it
    # softly let a missing file pass the build and fail silently at
    # runtime during postprocessing instead.
    require_file "${soc_models}/coco_80_labels_list.txt" "coco_80_labels_list.txt for ${soc} (run 'make build-models')"

    soc_dir="${STAGING_DIR}/${soc}"
    mkdir -p "${soc_dir}/bin" "${soc_dir}/lib" "${soc_dir}/model"

    install -m 0755 "${WATCHER_BIN}" "${soc_dir}/watcher"
    install -m 0755 "${ENGINE_BIN}" "${soc_dir}/argus-engine"
    # SoC-independent arm64 binary built by build-image-server (same binary in
    # every SoC dir). cmd/watcher/main.go's buildSpecs joins extDir with
    # previewBinary ("image-stream-server") directly, so it must land here.
    install -m 0755 "${IMAGE_SERVER_BIN}" "${soc_dir}/image-stream-server"
    # On-player zone dashboard (config-server, :20300). SoC-independent arm64 Go
    # binary with its web page embedded; the watcher supervises it. Same binary in
    # every SoC dir, like image-stream-server; main.go joins extDir with "config-server".
    install -m 0755 "${CONFIG_SERVER_BIN}" "${soc_dir}/config-server"
    # Optional MediaMTX proxy + its default config. SoC-independent arm64 binary,
    # staged into every SoC dir like image-stream-server. The watcher runs it only
    # when config.json opts in (mediamtx.enabled); cmd/watcher/main.go joins extDir
    # with "mediamtx" and falls back to this "mediamtx.yml" as the packaged default.
    install -m 0755 "${MEDIAMTX_DIR}/mediamtx" "${soc_dir}/mediamtx"
    install -m 0644 configs/mediamtx.yml "${soc_dir}/mediamtx.yml"
    # The single combined operator config (streams + mediamtx + engine template);
    # cmd/watcher/main.go discovers it (SD override first, this packaged copy last).
    install -m 0644 configs/config.json "${soc_dir}/config.json"
    log "  binaries: watcher, argus-engine, image-stream-server, mediamtx"

    # mosquitto broker + client tools (build-mosquitto) and the harvested runtime
    # libs (build-libs) are generic aarch64, so the same artifacts are staged into
    # every SoC dir.
    cp -a "${MOSQUITTO_DIR}/." "${soc_dir}/bin/"
    cp -a "${LIBS_DIR}/." "${soc_dir}/lib/"
    log "  mosquitto broker + runtime libs"

    # config.json's model_path uses the upstream filename RetinaFace.rknn
    # (capital), so the compiled models are staged under their real names.
    install -m 0644 "${soc_models}/RetinaFace.rknn" "${soc_dir}/model/RetinaFace.rknn"
    install -m 0644 "${soc_models}/yolox_s.rknn" "${soc_dir}/model/yolox_s.rknn"
    # engine/src/postprocess.cc: #define LABEL_NALE_TXT_PATH
    # "model/coco_80_labels_list.txt" -- read relative to the engine
    # process's cwd, which supervisor.ExecLauncher sets to extDir (this
    # per-SoC staging dir). Must land at exactly model/coco_80_labels_list.txt.
    install -m 0644 "${soc_models}/coco_80_labels_list.txt" "${soc_dir}/model/coco_80_labels_list.txt"
    log "  models: RetinaFace, YOLOX + COCO labels"
    ok "${soc} (${label}) staged"
done

# Remove older packages so the repo root does not accumulate stale zips. Done
# here (after staging succeeded, just before creating this build's zips) rather
# than at the start, so a failed staging run leaves the previous good zips intact.
# The prefix is derived from DEV_ZIP (e.g. argus-rtsp-watcher), and the glob
# matches both the dev (<prefix>-<ts>.zip) and ext (<prefix>-ext-<ts>.zip) zips.
log "Cleaning up old packages..."
zip_prefix="$(basename "${DEV_ZIP}" | sed 's/-[0-9][0-9]*\.zip$//')"
removed=0
shopt -s nullglob
for z in "${REPO_ROOT}/${zip_prefix}"-*.zip; do
    rm -f "${z}"
    removed=$((removed + 1))
done
shopt -u nullglob
ok "Removed ${removed} old package(s)"

log "Creating development package..."
rm -f "${DEV_ZIP}"
(cd "${STAGING_DIR}" && zip -rq "${REPO_ROOT}/${DEV_ZIP}" .)
ok "Development package created: ${DEV_ZIP} ($(human_size "${REPO_ROOT}/${DEV_ZIP}"))"

log "Creating production extension package (mksquashfs)..."
(cd "${STAGING_DIR}" && EXT_NAME="${EXT_NAME}" "${REPO_ROOT}/packaging/make-extension-lvm")

rm -f "${EXT_ZIP}"
(cd "${STAGING_DIR}" && zip -q "${REPO_ROOT}/${EXT_ZIP}" "ext_${EXT_NAME}.squashfs" "ext_${EXT_NAME}_install-lvm.sh")
rm -rf "${STAGING_DIR}/ext_${EXT_NAME}.squashfs" "${STAGING_DIR}/ext_${EXT_NAME}_install-lvm.sh"
ok "Extension package created: ${EXT_ZIP} ($(human_size "${REPO_ROOT}/${EXT_ZIP}"))"

# On-device install helper, regenerated each package and copied to /storage/sd
# alongside the zip by `make copy`. It embeds THIS build's exact zip name so it
# installs the right one even though /storage/sd accumulates old zips.
INSTALL_HELPER="${INSTALL_HELPER:-install-on-player.sh}"
ZIP_BASE="$(basename "${EXT_ZIP}")"
# Glob matching this project's extension zips (any timestamp), for pruning old ones.
ZIP_GLOB="$(echo "${ZIP_BASE}" | sed 's/-[0-9][0-9]*\.zip$/-*.zip/')"
log "Generating install helper..."
cat > "${REPO_ROOT}/${INSTALL_HELPER}" <<EOF
#!/bin/sh
# Generated by 'make package' for ${ZIP_BASE}. Do not edit -- regenerated each build.
#
# Run on the player's ROOT Linux shell. Descend there first from an SSH session:
#   Ctrl-C, Enter  ->  exit  ->  exit    (debugger -> BrightSign Shell -> root bash)
# then:
#   sh ${PLAYER_DEST:-/storage/sd}/${INSTALL_HELPER}
# On success the player reboots automatically; on failure it stays in the shell.

fail() { echo "ERROR: \$1" >&2; echo "Install FAILED -- staying in the shell (no reboot)." >&2; exit 1; }

cd /usr/local || fail "cannot cd /usr/local"
echo "==> Unpacking /storage/sd/${ZIP_BASE}"
unzip -o "/storage/sd/${ZIP_BASE}" || fail "unzip of ${ZIP_BASE} failed"
echo "==> Running ext_${EXT_NAME}_install-lvm.sh"
bash "./ext_${EXT_NAME}_install-lvm.sh" || fail "ext_${EXT_NAME}_install-lvm.sh failed"

# Install succeeded: prune older extension zips, keeping the one we just installed.
echo "==> Pruning old extension zips from /storage/sd (keeping ${ZIP_BASE})"
for z in /storage/sd/${ZIP_GLOB}; do
  [ -e "\$z" ] || continue
  [ "\$z" = "/storage/sd/${ZIP_BASE}" ] && continue
  echo "    rm \$z"
  rm -f "\$z"
done

echo "==> Install complete -- rebooting to activate."
reboot
EOF
chmod +x "${REPO_ROOT}/${INSTALL_HELPER}"
ok "Install helper generated: ${INSTALL_HELPER} (installs ${ZIP_BASE})"

DURATION=$(( $(date +%s) - START_TS ))
echo
ok "Packaging completed in $((DURATION / 60))m $((DURATION % 60))s"
echo
echo "Created packages:"
echo "  📦 ${DEV_ZIP} (development/testing)"
echo "  📦 ${EXT_ZIP} (production)"
echo
echo "Package contents:"
for soc in ${SOC_LIST}; do
    echo "  🎯 ${soc} ($(soc_label "${soc}")): watcher + engine + RetinaFace + YOLOX + mediamtx"
done
echo
echo "Next steps:"
echo "  1. make copy    # scp the newest zip + ${INSTALL_HELPER} to the player"
echo "  2. On the player's root Linux shell: sh /storage/sd/${INSTALL_HELPER}"
