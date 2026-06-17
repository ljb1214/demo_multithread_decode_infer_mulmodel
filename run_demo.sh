#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
BIN_PATH="${BIN_PATH:-${BUILD_DIR}/myDemo}"

if [[ -f "${PROJECT_ROOT}/demo.env" ]]; then
    # shellcheck disable=SC1091
    source "${PROJECT_ROOT}/demo.env"
fi

if [[ ! -x "${BIN_PATH}" ]]; then
    echo "Binary not found: ${BIN_PATH}"
    echo "Run ./build.sh first."
    exit 1
fi

export MYDEMO_ASSET_ROOT="${MYDEMO_ASSET_ROOT:-${PROJECT_ROOT}}"
export MYDEMO_GB_CONFIG="${MYDEMO_GB_CONFIG:-${PROJECT_ROOT}/gb28181.conf}"
export MYDEMO_MODEL_PERSON="${MYDEMO_MODEL_PERSON:-${PROJECT_ROOT}/assets/models/person_relu.rknn}"
export MYDEMO_MODEL_HELMET="${MYDEMO_MODEL_HELMET:-${PROJECT_ROOT}/assets/models/helmet_relu.rknn}"
export MYDEMO_MODEL_CALLPLAY="${MYDEMO_MODEL_CALLPLAY:-${PROJECT_ROOT}/assets/models/callplay_relu.rknn}"
export MYDEMO_LABEL_PATH="${MYDEMO_LABEL_PATH:-${PROJECT_ROOT}/assets/models/coco_80_labels_list.txt}"

if [[ $# -eq 0 ]]; then
    exec "${BIN_PATH}"
fi

exec "${BIN_PATH}" "$@"
