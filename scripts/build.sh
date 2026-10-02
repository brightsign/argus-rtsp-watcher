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
#   <SOC>/engine.template.json   per-stream engine config template
#   <SOC>/watcher.json           default operator config (from examples/watcher.example.json)
#   <SOC>/image-stream-server     MJPEG preview server (built by build-image-server;
#                                 one SoC-independent arm64 binary, staged into each SoC dir)
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
# Runtime assets produced on demand (no sibling argus tree): mosquitto broker +
# client tools, the harvested runtime lib/ tree, and the per-SoC compiled models.
MOSQUITTO_DIR="${MOSQUITTO_DIR:-${REPO_ROOT}/build/mosquitto}"
LIBS_DIR="${LIBS_DIR:-${REPO_ROOT}/build/libs}"
MODELS_DIR="${MODELS_DIR:-${REPO_ROOT}/build/models}"
EXT_NAME="${EXT_NAME:-npu_argus_rtsp}"
BUILD_TS="${BUILD_TS:-$(date +%s)}"
DEV_ZIP="${DEV_ZIP:-argus-rtsp-watcher-${BUILD_TS}.zip}"
EXT_ZIP="${EXT_ZIP:-argus-rtsp-watcher-ext-${BUILD_TS}.zip}"

require_file() {
    local path="$1" what="$2" hint="${3:-}"
    if [ ! -f "${path}" ]; then
        echo "ERROR: missing ${what}: ${path}" >&2
        [ -n "${hint}" ] && echo "  ${hint}" >&2
        exit 1
    fi
}

require_dir() {
    local path="$1" what="$2" hint="${3:-}"
    if [ ! -d "${path}" ]; then
        echo "ERROR: missing ${what}: ${path}" >&2
        [ -n "${hint}" ] && echo "  ${hint}" >&2
        exit 1
    fi
}

require_file "${WATCHER_BIN}" "watcher binary (run 'make build-watcher')"
require_file "${ENGINE_BIN}" "engine binary (run 'make build-engine')"
require_file "${IMAGE_SERVER_BIN}" "image-stream-server binary (run 'make build-image-server')"
require_file "${MOSQUITTO_DIR}/mosquitto" "mosquitto broker (run 'make build-mosquitto')"
require_dir "${LIBS_DIR}" "harvested runtime libs (run 'make build-libs')"
require_file "packaging/bsext_init" "bsext_init"
require_file "packaging/make-extension-lvm" "make-extension-lvm"
require_file "configs/engine.template.json" "engine template config"
require_file "examples/watcher.example.json" "default watcher config"

echo "==> Cleaning ${STAGING_DIR}"
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"

install -m 0755 packaging/bsext_init "${STAGING_DIR}/bsext_init"

for soc in ${SOC_LIST}; do
    echo "==> Staging ${soc}"
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
    install -m 0644 configs/engine.template.json "${soc_dir}/engine.template.json"
    install -m 0644 examples/watcher.example.json "${soc_dir}/watcher.json"

    # mosquitto broker + client tools (build-mosquitto) and the harvested runtime
    # libs (build-libs) are generic aarch64, so the same artifacts are staged into
    # every SoC dir.
    cp -a "${MOSQUITTO_DIR}/." "${soc_dir}/bin/"
    cp -a "${LIBS_DIR}/." "${soc_dir}/lib/"

    # engine.template.json's model_path uses the upstream filename RetinaFace.rknn
    # (capital), so the compiled models are staged under their real names.
    install -m 0644 "${soc_models}/RetinaFace.rknn" "${soc_dir}/model/RetinaFace.rknn"
    install -m 0644 "${soc_models}/yolox_s.rknn" "${soc_dir}/model/yolox_s.rknn"
    # engine/src/postprocess.cc: #define LABEL_NALE_TXT_PATH
    # "model/coco_80_labels_list.txt" -- read relative to the engine
    # process's cwd, which supervisor.ExecLauncher sets to extDir (this
    # per-SoC staging dir). Must land at exactly model/coco_80_labels_list.txt.
    install -m 0644 "${soc_models}/coco_80_labels_list.txt" "${soc_dir}/model/coco_80_labels_list.txt"
done

echo "==> Creating dev package ${DEV_ZIP}"
rm -f "${DEV_ZIP}"
(cd "${STAGING_DIR}" && zip -rq "${REPO_ROOT}/${DEV_ZIP}" .)

echo "==> Running packaging/make-extension-lvm (needs mksquashfs)"
if ! command -v mksquashfs >/dev/null 2>&1; then
    echo "ERROR: mksquashfs not found; install squashfs-tools to build the LVM extension package" >&2
    exit 1
fi
(cd "${STAGING_DIR}" && EXT_NAME="${EXT_NAME}" "${REPO_ROOT}/packaging/make-extension-lvm")

echo "==> Creating extension package ${EXT_ZIP}"
rm -f "${EXT_ZIP}"
(cd "${STAGING_DIR}" && zip -q "${REPO_ROOT}/${EXT_ZIP}" "ext_${EXT_NAME}.squashfs" "ext_${EXT_NAME}_install-lvm.sh")
rm -rf "${STAGING_DIR}/ext_${EXT_NAME}.squashfs" "${STAGING_DIR}/ext_${EXT_NAME}_install-lvm.sh"

echo "==> Done"
echo "  dev package:       ${DEV_ZIP}"
echo "  extension package: ${EXT_ZIP}"
