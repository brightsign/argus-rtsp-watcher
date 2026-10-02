#!/usr/bin/env bash
set -euo pipefail

# Compiles the RKNN models (RetinaFace + YOLOX) per SoC from their upstream ONNX
# sources, so this project does not depend on a sibling argus install tree for
# them. Mirrors argus's compile-models: clone rknn-toolkit2 + rknn_model_zoo
# (pinned), build the rknn_tk2 container image, then run convert.py per SoC.
#
# Invoked by `make build-models` (and therefore `make build`). Needs docker + a
# network + ~10GB for the toolkit image. The compiled .rknn are SoC-specific
# (quantized per NPU), so one set is produced per SoC under <output-dir>/<SOC>/.
#
# Usage: build-models.sh <output-dir> [SOC ...]   (default SOCs: RK3588 RK3576 RK3568)

OUTPUT_DIR="${1:?usage: build-models.sh <output-dir> [SOC ...]}"
shift || true
SOCS=("$@")
if [ "${#SOCS[@]}" -eq 0 ]; then
    SOCS=(RK3588 RK3576 RK3568)
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT_DIR="${REPO_ROOT}/build/toolkit"
ZOO_DIR="${TOOLKIT_DIR}/rknn_model_zoo"
TK2_DIR="${TOOLKIT_DIR}/rknn-toolkit2"
TOOLKIT_REF="v2.3.0"
CONTAINER="docker"
IMAGE="rknn_tk2"

# model key -> "<example-dir> <onnx-name> <output-rknn-name>"
declare -A MODELS=(
    ["retinaface"]="RetinaFace RetinaFace_mobile320.onnx RetinaFace.rknn"
    ["yolox"]="yolox yolox_s.onnx yolox_s.rknn"
)

require_docker() {
    command -v "${CONTAINER}" >/dev/null 2>&1 || { echo "ERROR: ${CONTAINER} is required but not installed" >&2; exit 1; }
    "${CONTAINER}" info >/dev/null 2>&1 || { echo "ERROR: ${CONTAINER} daemon is not running" >&2; exit 1; }
}

clone_toolkit() {
    mkdir -p "${TOOLKIT_DIR}"
    if [ ! -d "${TK2_DIR}" ]; then
        echo "==> Cloning rknn-toolkit2 (${TOOLKIT_REF})"
        git clone --depth 1 --branch "${TOOLKIT_REF}" https://github.com/airockchip/rknn-toolkit2.git "${TK2_DIR}"
    fi
    if [ ! -d "${ZOO_DIR}" ]; then
        echo "==> Cloning rknn_model_zoo (${TOOLKIT_REF})"
        git clone --depth 1 --branch "${TOOLKIT_REF}" https://github.com/airockchip/rknn_model_zoo.git "${ZOO_DIR}"
    fi
}

build_image() {
    if "${CONTAINER}" image inspect "${IMAGE}" >/dev/null 2>&1; then
        echo "==> ${IMAGE} image present"
        return
    fi
    local dockerfile_dir="${TK2_DIR}/rknn-toolkit2/docker/docker_file/ubuntu_20_04_cp38"
    local dockerfile="Dockerfile_ubuntu_20_04_for_cp38"
    if [ ! -f "${dockerfile_dir}/${dockerfile}" ]; then
        echo "ERROR: toolkit Dockerfile not found at ${dockerfile_dir}/${dockerfile}" >&2
        exit 1
    fi
    echo "==> Building ${IMAGE} image (large; one-time)"
    ( cd "${dockerfile_dir}" && "${CONTAINER}" build --rm -t "${IMAGE}" -f "${dockerfile}" . )
}

download_onnx() {
    local example_dir="$1" onnx="$2"
    local model_dir="${ZOO_DIR}/examples/${example_dir}/model"
    if [ -f "${model_dir}/${onnx}" ]; then
        return
    fi
    echo "==> Downloading ${example_dir} ONNX"
    if [ ! -f "${model_dir}/download_model.sh" ]; then
        echo "ERROR: download script not found: ${model_dir}/download_model.sh" >&2
        exit 1
    fi
    ( cd "${model_dir}" && chmod +x ./download_model.sh && ./download_model.sh )
}

compile_one() {
    local example_dir="$1" onnx="$2" out="$3" soc_upper="$4"
    local soc_lower="${soc_upper,,}"
    echo "==> Compiling ${example_dir} for ${soc_upper}"
    mkdir -p "${ZOO_DIR}/examples/${example_dir}/model/${soc_upper}"
    # No TTY under make; convert.py <onnx> <soc> <dtype> <out>, run inside /zoo.
    "${CONTAINER}" run --rm -v "${ZOO_DIR}:/zoo" "${IMAGE}" /bin/bash -c \
        "cd /zoo/examples/${example_dir}/python && python convert.py ../model/${onnx} ${soc_lower} i8 ../model/${soc_upper}/${out}"
    if [ ! -f "${ZOO_DIR}/examples/${example_dir}/model/${soc_upper}/${out}" ]; then
        echo "ERROR: compilation produced no ${out} for ${soc_upper}" >&2
        exit 1
    fi
}

stage_outputs() {
    local soc_upper="$1"
    local dst="${OUTPUT_DIR}/${soc_upper}"
    mkdir -p "${dst}"
    install -m 0644 "${ZOO_DIR}/examples/RetinaFace/model/${soc_upper}/RetinaFace.rknn" "${dst}/RetinaFace.rknn"
    install -m 0644 "${ZOO_DIR}/examples/yolox/model/${soc_upper}/yolox_s.rknn" "${dst}/yolox_s.rknn"
    # postprocess.cc reads coco_80_labels_list.txt with spaces converted to
    # underscores (argus does the same sed transform).
    sed 's/ /_/g' "${ZOO_DIR}/examples/yolox/model/coco_80_labels_list.txt" > "${dst}/coco_80_labels_list.txt"
    echo "==> models -> ${dst}"
}

models_present() {
    local soc_upper="$1"
    [ -f "${OUTPUT_DIR}/${soc_upper}/RetinaFace.rknn" ] &&
        [ -f "${OUTPUT_DIR}/${soc_upper}/yolox_s.rknn" ] &&
        [ -f "${OUTPUT_DIR}/${soc_upper}/coco_80_labels_list.txt" ]
}

main() {
    local pending=()
    local soc
    for soc in "${SOCS[@]}"; do
        if models_present "${soc}"; then
            echo "==> ${soc} models present, skipping"
        else
            pending+=("${soc}")
        fi
    done
    if [ "${#pending[@]}" -eq 0 ]; then
        echo "==> all models already compiled"
        return 0
    fi

    require_docker
    clone_toolkit
    build_image

    local key spec
    for key in "${!MODELS[@]}"; do
        read -r example_dir onnx _out <<<"${MODELS[$key]}"
        download_onnx "${example_dir}" "${onnx}"
    done

    for soc in "${pending[@]}"; do
        for key in "${!MODELS[@]}"; do
            read -r example_dir onnx out <<<"${MODELS[$key]}"
            compile_one "${example_dir}" "${onnx}" "${out}" "${soc}"
        done
        stage_outputs "${soc}"
    done
}

main "$@"
