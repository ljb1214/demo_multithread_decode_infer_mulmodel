cmake_minimum_required(VERSION 3.4.1)
project(rk_platform)

# ================= 编译选项 =================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-pthread")

# ================= 项目根路径 =================
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# 兼容老的 project2 目录布局，作为 GB28181 依赖的兜底来源
set(PROJECT2_ROOT_DEFAULT ${PROJECT_ROOT}/../../project2)
if(EXISTS ${PROJECT_ROOT}/../project2 AND NOT EXISTS ${PROJECT2_ROOT_DEFAULT})
    set(PROJECT2_ROOT_DEFAULT ${PROJECT_ROOT}/../project2)
endif()
if(DEFINED ENV{PROJECT2_ROOT} AND NOT "$ENV{PROJECT2_ROOT}" STREQUAL "")
    set(PROJECT2_ROOT_DEFAULT $ENV{PROJECT2_ROOT})
endif()

function(set_cache_path_from_candidates VAR DESC)
    set(_candidates ${ARGN})
    set(_detected "")

    if(DEFINED ${VAR} AND NOT "${${VAR}}" STREQUAL "")
        set(_detected "${${VAR}}")
    else()
        foreach(_candidate IN LISTS _candidates)
            if(NOT "${_candidate}" STREQUAL "" AND EXISTS "${_candidate}")
                set(_detected "${_candidate}")
                break()
            endif()
        endforeach()
    endif()

    set(${VAR} "${_detected}" CACHE PATH "${DESC}" FORCE)
endfunction()

set(THIRD_PARTY_ROOT "${PROJECT_ROOT}/third_party")
set(RK_SDK_ROOT "$ENV{RK_SDK_ROOT}" CACHE PATH "Optional root that contains rknn/mpp/rga/ffmpeg-lite/opencv4-1/gb28181")

# ================= RK 相关库 =================
set_cache_path_from_candidates(RKNN_API_PATH "Path to RKNN runtime package"
    "$ENV{RKNN_API_PATH}"
    "${THIRD_PARTY_ROOT}/rknn"
    "${RK_SDK_ROOT}/rknn"
    "${PROJECT_ROOT}/../rknn"
)
set_cache_path_from_candidates(MPP_PATH "Path to Rockchip MPP package"
    "$ENV{MPP_PATH}"
    "${THIRD_PARTY_ROOT}/mpp"
    "${RK_SDK_ROOT}/mpp"
    "${PROJECT_ROOT}/../mpp"
)
set_cache_path_from_candidates(RGA_PATH "Path to Rockchip RGA package"
    "$ENV{RGA_PATH}"
    "${THIRD_PARTY_ROOT}/rga"
    "${RK_SDK_ROOT}/rga"
    "${PROJECT_ROOT}/../rga"
)

set(LIB_RKNN_RT   ${RKNN_API_PATH}/lib/librknnrt.so)
set(LIB_RKNN_API  ${RKNN_API_PATH}/lib/librknn_api.so)
set(LIB_RKMPP     ${MPP_PATH}/lib/librockchip_mpp.so)
set(LIB_RGA       ${RGA_PATH}/lib/librga.so)

# ================= FFmpeg（优先本地，允许通过缓存变量/环境变量切换） =================
set_cache_path_from_candidates(FFMPEG_ROOT "Path to ffmpeg-lite package"
    "$ENV{FFMPEG_ROOT}"
    "${THIRD_PARTY_ROOT}/ffmpeg-lite"
    "${RK_SDK_ROOT}/ffmpeg-lite"
    "${PROJECT_ROOT}/../ffmpeg-lite"
)
set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT}/include)
set(FFMPEG_LIB_DIR     ${FFMPEG_ROOT}/lib)

# 检查FFmpeg目录是否存在
if(NOT EXISTS ${FFMPEG_ROOT})
    message(FATAL_ERROR "FFmpeg directory not found: ${FFMPEG_ROOT}")
endif()

# 检查 FFmpeg 头文件和库目录是否存在
if(NOT EXISTS ${FFMPEG_INCLUDE_DIR})
    message(FATAL_ERROR "FFmpeg include directory not found: ${FFMPEG_INCLUDE_DIR}")
endif()

if(NOT EXISTS ${FFMPEG_LIB_DIR})
    message(FATAL_ERROR "FFmpeg lib directory not found: ${FFMPEG_LIB_DIR}")
endif()

message(STATUS "Using ffmpeg-lite from: ${FFMPEG_ROOT}")

# ================= OpenCV（优先本地，允许通过缓存变量/环境变量切换） =================
set_cache_path_from_candidates(OPENCV_ROOT "Path to OpenCV package"
    "$ENV{OPENCV_ROOT}"
    "${THIRD_PARTY_ROOT}/opencv4-1"
    "${RK_SDK_ROOT}/opencv4-1"
    "${PROJECT_ROOT}/../opencv4-1"
)
set(OPENCV_INCLUDE_DIR ${OPENCV_ROOT}/include)
set(OPENCV_INCLUDE_OPENCV4_DIR ${OPENCV_ROOT}/include/opencv4)
set(OPENCV_LIB_DIR     ${OPENCV_ROOT}/lib)

# 检查OpenCV目录是否存在
if(NOT EXISTS ${OPENCV_ROOT})
    message(FATAL_ERROR "OpenCV directory not found: ${OPENCV_ROOT}")
endif()

# 检查OpenCV头文件目录
if(NOT EXISTS ${OPENCV_INCLUDE_DIR})
    message(FATAL_ERROR "OpenCV include directory not found: ${OPENCV_INCLUDE_DIR}")
endif()

# 检查opencv4子目录
if(NOT EXISTS ${OPENCV_INCLUDE_OPENCV4_DIR})
    message(FATAL_ERROR "OpenCV opencv4 include directory not found: ${OPENCV_INCLUDE_OPENCV4_DIR}")
endif()

# 检查OpenCV库目录
if(NOT EXISTS ${OPENCV_LIB_DIR})
    message(FATAL_ERROR "OpenCV lib directory not found: ${OPENCV_LIB_DIR}")
endif()

foreach(_rk_lib ${LIB_RKNN_RT} ${LIB_RKNN_API} ${LIB_RKMPP} ${LIB_RGA})
    if(NOT EXISTS ${_rk_lib})
        message(FATAL_ERROR "Required Rockchip library not found: ${_rk_lib}")
    endif()
endforeach()

# 查找OpenCV库文件
file(GLOB OpenCV_LIBS_FILES "${OPENCV_LIB_DIR}/libopencv_*.so")
if(NOT OpenCV_LIBS_FILES)
    message(FATAL_ERROR "No OpenCV library files found in ${OPENCV_LIB_DIR}")
endif()

set(OpenCV_LIBS ${OpenCV_LIBS_FILES})
list(LENGTH OpenCV_LIBS_FILES OPENCV_LIBS_COUNT)
message(STATUS "OpenCV found: ${OPENCV_ROOT} (${OPENCV_LIBS_COUNT} libraries)")

# ================= 头文件 =================
include_directories(
    ${PROJECT_ROOT}
    ${PROJECT_ROOT}/include

    ${RKNN_API_PATH}/include
    ${MPP_PATH}/include
    ${RGA_PATH}/include

    ${FFMPEG_INCLUDE_DIR}
    ${OPENCV_INCLUDE_DIR}
    ${OPENCV_INCLUDE_OPENCV4_DIR}
)

# ================= demo 源码 =================
file(GLOB DEMO_LIB_SOURCES
    ${PROJECT_ROOT}/lib/*.cpp
)

set(LOCAL_GB_SOURCES
    ${PROJECT_ROOT}/lib/config.cc
    ${PROJECT_ROOT}/lib/decode.cc
    ${PROJECT_ROOT}/lib/gb28181.cc
    ${PROJECT_ROOT}/lib/ps_rtp.cc
    ${PROJECT_ROOT}/lib/push.cc
    ${PROJECT_ROOT}/lib/xml_utils.cc
)

list(REMOVE_ITEM DEMO_LIB_SOURCES ${LOCAL_GB_SOURCES})

# GB28181 依赖优先使用项目内 third_party/gb28181，也兼容旧的 project2/install 布局
set_cache_path_from_candidates(GB28181_ROOT "Path to GB28181 dependency package"
    "$ENV{GB28181_ROOT}"
    "${THIRD_PARTY_ROOT}/gb28181"
    "${RK_SDK_ROOT}/gb28181"
    "${PROJECT_ROOT}/../gb28181"
    "${PROJECT2_ROOT_DEFAULT}/install"
)
set(GB_SOURCES "")
set(GB28181_LIBS "")
set(GB28181_INCLUDE_DIRS "")
set(GB28181_DEFINES "")

set(ENABLE_GB28181 AUTO CACHE STRING "Enable GB28181 support: AUTO/ON/OFF")
set_property(CACHE ENABLE_GB28181 PROPERTY STRINGS AUTO ON OFF)

set(_want_gb FALSE)
if(ENABLE_GB28181 STREQUAL "ON")
    set(_want_gb TRUE)
elseif(ENABLE_GB28181 STREQUAL "AUTO")
    if(EXISTS ${GB28181_ROOT})
        set(_want_gb TRUE)
    endif()
endif()

if(_want_gb)
    unset(EXOSIP_INCLUDE_DIR CACHE)
    unset(EXOSIP_LIB CACHE)
    unset(OSIP2_LIB CACHE)
    unset(OSIPPARSER2_LIB CACHE)

    find_path(EXOSIP_INCLUDE_DIR eXosip2/eXosip.h
        PATHS ${GB28181_ROOT}/include
        NO_DEFAULT_PATH
    )
    find_library(EXOSIP_LIB eXosip2
        PATHS ${GB28181_ROOT}/lib
        NO_DEFAULT_PATH
    )
    find_library(OSIP2_LIB osip2
        PATHS ${GB28181_ROOT}/lib
        NO_DEFAULT_PATH
    )
    find_library(OSIPPARSER2_LIB osipparser2
        PATHS ${GB28181_ROOT}/lib
        NO_DEFAULT_PATH
    )

    set(GB28181_SSL_LIB "")
    set(GB28181_CRYPTO_LIB "")
    if(EXISTS "${GB28181_ROOT}/lib/libssl.so.1.1")
        set(GB28181_SSL_LIB "${GB28181_ROOT}/lib/libssl.so.1.1")
    endif()
    if(EXISTS "${GB28181_ROOT}/lib/libcrypto.so.1.1")
        set(GB28181_CRYPTO_LIB "${GB28181_ROOT}/lib/libcrypto.so.1.1")
    endif()

    # 如果没找到，再使用默认路径查找
    if(NOT EXOSIP_INCLUDE_DIR)
        find_path(EXOSIP_INCLUDE_DIR eXosip2/eXosip.h)
    endif()
    if(NOT EXOSIP_LIB)
        find_library(EXOSIP_LIB eXosip2)
    endif()
    if(NOT OSIP2_LIB)
        find_library(OSIP2_LIB osip2)
    endif()
    if(NOT OSIPPARSER2_LIB)
        find_library(OSIPPARSER2_LIB osipparser2)
    endif()

    if(EXOSIP_INCLUDE_DIR AND EXOSIP_LIB AND OSIP2_LIB AND OSIPPARSER2_LIB)
        message(STATUS "GB28181 dependencies found: ${GB28181_ROOT}")
        message(STATUS "eXosip include: ${EXOSIP_INCLUDE_DIR}")

        set(GB_SOURCES ${LOCAL_GB_SOURCES})
        set(GB28181_INCLUDE_DIRS
            ${EXOSIP_INCLUDE_DIR}
        )
        set(GB28181_DEFINES ENABLE_GB28181)

        set(GB28181_LIBS
            ${EXOSIP_LIB}
            ${OSIP2_LIB}
            ${OSIPPARSER2_LIB}
            ${GB28181_SSL_LIB}
            ${GB28181_CRYPTO_LIB}
        )
    else()
        if(NOT EXISTS ${GB28181_ROOT})
            message(WARNING "GB28181 dependency package not found at: ${GB28181_ROOT}. Building without GB28181 sources (RTMP still works).")
        else()
            message(WARNING "GB28181 package found but eXosip/osip headers/libs not found. Building without GB28181 sources (RTMP still works).")
            message(STATUS "EXOSIP_INCLUDE_DIR=${EXOSIP_INCLUDE_DIR}")
            message(STATUS "EXOSIP_LIB=${EXOSIP_LIB}")
            message(STATUS "OSIP2_LIB=${OSIP2_LIB}")
            message(STATUS "OSIPPARSER2_LIB=${OSIPPARSER2_LIB}")
        endif()
    endif()
else()
    message(STATUS "GB28181 disabled (ENABLE_GB28181=${ENABLE_GB28181})")
endif()

add_executable(myDemo
    main.cpp
    ${DEMO_LIB_SOURCES}
    ${GB_SOURCES}
)

target_include_directories(myDemo PRIVATE ${GB28181_INCLUDE_DIRS})
target_compile_definitions(myDemo PRIVATE ${GB28181_DEFINES})
target_link_options(myDemo PRIVATE -Wl,--disable-new-dtags)

# ================= 链接（全部用绝对路径） =================
target_link_libraries(myDemo
    # -------- FFmpeg --------
    ${FFMPEG_LIB_DIR}/libavcodec.so
    ${FFMPEG_LIB_DIR}/libavformat.so
    ${FFMPEG_LIB_DIR}/libavutil.so
    # 如果你的 ffmpeg-lite 只构建了 avformat/avcodec/avutil，可以把多余的几个库去掉

    # -------- OpenCV --------
    ${OpenCV_LIBS}

    # -------- Rockchip --------
    ${LIB_RKMPP}
    ${LIB_RGA}
    ${LIB_RKNN_API}
    ${LIB_RKNN_RT}
    -Wl,--no-as-needed
    ${GB28181_LIBS}
    -Wl,--as-needed
)


set(RUNTIME_RPATH_LIST
    ${FFMPEG_LIB_DIR}
    ${OPENCV_LIB_DIR}
    ${RKNN_API_PATH}/lib
    ${MPP_PATH}/lib
    ${RGA_PATH}/lib
)

if(EXISTS "${GB28181_ROOT}/lib")
    list(APPEND RUNTIME_RPATH_LIST "${GB28181_ROOT}/lib")
endif()

list(APPEND RUNTIME_RPATH_LIST /usr/lib/aarch64-linux-gnu)  # 系统库路径，用于x264/x265

set_target_properties(myDemo PROPERTIES
    BUILD_RPATH "${RUNTIME_RPATH_LIST}"
    INSTALL_RPATH "${RUNTIME_RPATH_LIST}"
)
