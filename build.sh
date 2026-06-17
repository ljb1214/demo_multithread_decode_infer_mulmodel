#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
THIRD_PARTY_ROOT="${PROJECT_ROOT}/third_party"

find_first_existing_dir() {
    local candidate
    for candidate in "$@"; do
        if [[ -n "${candidate}" && -d "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

RKNN_API_PATH="${RKNN_API_PATH:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/rknn" \
    "${PROJECT_ROOT}/../rknn" \
    "${RK_SDK_ROOT:-}/rknn" || true)}"
MPP_PATH="${MPP_PATH:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/mpp" \
    "${PROJECT_ROOT}/../mpp" \
    "${RK_SDK_ROOT:-}/mpp" || true)}"
RGA_PATH="${RGA_PATH:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/rga" \
    "${PROJECT_ROOT}/../rga" \
    "${RK_SDK_ROOT:-}/rga" || true)}"
FFMPEG_ROOT="${FFMPEG_ROOT:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/ffmpeg-lite" \
    "${PROJECT_ROOT}/../ffmpeg-lite" \
    "${RK_SDK_ROOT:-}/ffmpeg-lite" || true)}"
OPENCV_ROOT="${OPENCV_ROOT:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/opencv4-1" \
    "${PROJECT_ROOT}/../opencv4-1" \
    "${RK_SDK_ROOT:-}/opencv4-1" || true)}"
GB28181_ROOT="${GB28181_ROOT:-$(find_first_existing_dir \
    "${THIRD_PARTY_ROOT}/gb28181" \
    "${PROJECT_ROOT}/../gb28181" \
    "${RK_SDK_ROOT:-}/gb28181" \
    "${PROJECT_ROOT}/../../project2/install" \
    "${PROJECT_ROOT}/../project2/install" || true)}"

ENABLE_GB28181="${ENABLE_GB28181:-AUTO}"

echo "Project root : ${PROJECT_ROOT}"
echo "Build dir    : ${BUILD_DIR}"
echo "RKNN path    : ${RKNN_API_PATH:-<not found>}"
echo "MPP path     : ${MPP_PATH:-<not found>}"
echo "RGA path     : ${RGA_PATH:-<not found>}"
echo "FFmpeg path  : ${FFMPEG_ROOT:-<not found>}"
echo "OpenCV path  : ${OPENCV_ROOT:-<not found>}"
echo "GB28181 path : ${GB28181_ROOT:-<not found>}"
echo "GB28181      : ${ENABLE_GB28181}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DRKNN_API_PATH="${RKNN_API_PATH}" \
    -DMPP_PATH="${MPP_PATH}" \
    -DRGA_PATH="${RGA_PATH}" \
    -DFFMPEG_ROOT="${FFMPEG_ROOT}" \
    -DOPENCV_ROOT="${OPENCV_ROOT}" \
    -DGB28181_ROOT="${GB28181_ROOT}" \
    -DENABLE_GB28181="${ENABLE_GB28181}" \
    "$@"

cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

echo
echo "Build finished: ${BUILD_DIR}/myDemo"
