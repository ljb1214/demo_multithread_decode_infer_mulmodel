/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#include "mpp_encoder.h"           // MPP 编码器封装类
#include <opencv2/opencv.hpp>      // opencv 用于 BGR -> YUV 转换
#include <opencv2/imgproc.hpp>     // opencv 图像处理模块
#include "im2d.h"                  // RGA 硬件加速库
#include <stdio.h>                 // 标准输入输出
#include <stdlib.h>                // 标准库函数
#include <string.h>                // 字符串处理函数
#include <errno.h>                 // 错误码定义宏
#include <thread>                  // C++11 线程库，用于编码轮询等待
#include <chrono>                  // C++11 时间库，用于编码轮询等待

// MPP 编码器实现
MppEncoder::MppEncoder()
    : mpp_ctx_(NULL), mpp_mpi_(NULL), enc_cfg_(NULL),
      frame_(NULL), packet_(NULL), frm_grp_(NULL), pkt_grp_(NULL),
      width_(0), height_(0), fps_(0), bitrate_(0),
      mpp_type_(MPP_VIDEO_CodingAVC), initialized_(false),
      yuv_buffer_(NULL), yuv_buffer_size_(0), yuv_dma_fd_(-1) {
}
// 析构函数，确保资源释放
MppEncoder::~MppEncoder() {
    Release();
}

// 分配 dma-buf 内存，返回 fd，并通过 out_ptr 返回映射后的虚拟地址
static int alloc_dma_buffer(size_t size, uint8_t** out_ptr) {
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        return -1;
    }

    // 使用统一初始化，避免顺序问题
    struct drm_mode_create_dumb create = {};
    create.width = (uint32_t)size;
    create.height = 1;
    create.bpp = 8;
    create.flags = 0;

    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(drm_fd);
        return -1;
    }

    // 获取 dma-buf fd（使用 ioctl 直接调用，避免 drmPrimeHandleToFD 未声明）
    int fd = -1;
    struct drm_prime_handle prime_handle = {
        .handle = create.handle,
        .flags = DRM_CLOEXEC,
        .fd = -1,
    };
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) != 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        close(drm_fd);
        return -1;
    }
    fd = prime_handle.fd;

    // 映射虚拟地址（可选，用于 CPU 访问）
    struct drm_mode_map_dumb map = {};
    map.handle = create.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        close(fd);
        close(drm_fd);
        return -1;
    }
    void* ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        close(drm_fd);
        return -1;
    }
    *out_ptr = (uint8_t*)ptr;

    close(drm_fd);
    return fd;
}

// 初始化编码器，设置参数并准备资源，参数包括图像宽高、帧率、码率和编码格式（264/265）
int MppEncoder::Init(int width, int height, int fps, int bitrate, int codec_type) {
    MPP_RET ret = MPP_OK;
    
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;

    if (codec_type == 264) {
        mpp_type_ = MPP_VIDEO_CodingAVC;
    } else if (codec_type == 265) {
        mpp_type_ = MPP_VIDEO_CodingHEVC;
    } else {
        fprintf(stderr, "Unsupported codec type: %d\n", codec_type);
        return -1;
    }

    // 创建 MPP 上下文，作用是·协调编码器的生命周期和资源管理；MPI 是接口函数集合，提供编码控制和数据传输功能
    ret = mpp_create(&mpp_ctx_, &mpp_mpi_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_create failed ret %d\n", ret);
        return -1;
    }

    // 初始化编码器，参数包括上下文、编码类型（编码器/解码器）和视频编码格式（264/265）
    ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, mpp_type_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_init failed ret %d\n", ret);
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
        return -1;
    }

    // 创建编码配置，用于设置编码参数，如图像尺寸、码率控制、编码级别等
    ret = mpp_enc_cfg_init(&enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_enc_cfg_init failed ret %d\n", ret);
        goto FAIL;
    }

    // 获取默认配置,以便在此基础上修改特定参数，确保其他参数保持合理默认值
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_CFG, enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "get enc cfg failed ret %d\n", ret);
        goto FAIL;
    }

    // 基本图像参数,包括宽高、像素格式和 stride（行跨度），确保编码器正确处理输入数据
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:width", width);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:height", height);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:hor_stride", width);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:format", MPP_FMT_YUV420P);

    // 码率控制：简单的 VBR 设置，偏向速度,适合实时编码；更复杂的控制可以根据需要调整，如 CBR、固定 QP 等
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_max", bitrate * 2);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_min", bitrate / 2);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:gop", fps);
    // 264/265 特定参数,如 H.264 的 profile/level、QP 范围等，确保编码器按照预期的编码规范工作
    if (mpp_type_ == MPP_VIDEO_CodingAVC) {
        mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", MPP_VIDEO_CodingAVC);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:profile", 66);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:level", 40);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:cabac_en", 0);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_init", 26);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_min", 20);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_max", 35);
    // 265 的 profile/level 设置与 264 不同，且 HEVC 支持更多的编码特性，如更高的压缩效率和更复杂的编码工具，因此配置项也有所不同
    } else if (mpp_type_ == MPP_VIDEO_CodingHEVC) {
        mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", MPP_VIDEO_CodingHEVC);
        mpp_enc_cfg_set_s32(enc_cfg_, "h265:profile", 1);
        mpp_enc_cfg_set_s32(enc_cfg_, "h265:level", 120);
    }

    // 应用配置,将设置好的编码参数传递给编码器，确保编码器按照这些参数进行编码；如果配置无效或不被支持，编码器可能会返回错误
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_SET_CFG, enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "set enc cfg failed ret %d\n", ret);
        goto FAIL;
    }

    // 输入/输出缓冲区组,用于管理编码器使用的内存缓冲区，通常使用 ION 内存分配器以获得更好的性能和兼容性；如果缓冲区组创建失败，编码器将无法正常工作
    ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        fprintf(stderr, "failed to get buffer group for input frame ret %d\n", ret);
        goto FAIL;
    }
    // 输出缓冲区组通常需要与编码器的输出机制兼容，以确保编码后的数据能够正确传输和存储；如果输出缓冲区组创建失败，编码器将无法输出数据
    ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        fprintf(stderr, "failed to get buffer group for output packet ret %d\n", ret);
        goto FAIL;
    }

    // 预分配 YUV 缓冲区，使用 dma-buf 以获得 RGA 兼容的内存
    yuv_buffer_size_ = width * height * 3 / 2;

    // 分配 dma-buf，同时获取 fd 和映射的虚拟地址
    yuv_dma_fd_ = alloc_dma_buffer(yuv_buffer_size_, &yuv_buffer_);
        if (yuv_dma_fd_ < 0) {
    fprintf(stderr, "failed to allocate dma-buf for YUV\n");
        goto FAIL;
}   
// 编码器初始化成功，设置标志位
    initialized_ = true;
    return 0;

FAIL:
    // 释放的三个资源（yuv_buffer_、enc_cfg_、mpp_ctx_）是编码器初始化过程中可能成功分配的资源
    if (yuv_buffer_) {
        free(yuv_buffer_);
        yuv_buffer_ = NULL;
        yuv_buffer_size_ = 0;
    }
    if (enc_cfg_) {
        mpp_enc_cfg_deinit(enc_cfg_);
        enc_cfg_ = NULL;
    }
    if (mpp_ctx_) {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
    }
    initialized_ = false;
    return -1;
}

// 编码一帧图像，输入为 BGR 数据和尺寸信息，输出为编码后的数据包和大小；函数内部会先将 BGR 数据转换为 YUV420P 格式
// 然后调用 Encode() 函数进行编码；如果输入数据格式不正确或编码失败，函数会返回错误码
// 参数分别是：BGR 数据指针、图像宽高、输出数据包指针、输出数据包大小指针，以及 BGR 数据的行跨度（可选，默认为宽度 * 3）
int MppEncoder::EncodeFrame(uint8_t* bgr_data, int width, int height,
                            uint8_t* packet_data, int* packet_size, int bgr_stride) {
    if (!initialized_) {
        fprintf(stderr, "Encoder not initialized\n");
        return -1;
    }
    // 检查输入图像尺寸是否与初始化时设置的尺寸匹配，编码器通常要求输入图像尺寸固定，如果不匹配可能会导致编码失败或输出错误的结果
    if (width != width_ || height != height_) {
        fprintf(stderr, "Frame size mismatch: %dx%d vs %dx%d\n",
                width, height, width_, height_);
        return -1;
    }
    // 检查 YUV 缓冲区是否已分配且足够大，编码器需要一个足够大的缓冲区来存储转换后的 YUV 数据，如果缓冲区不足可能会导致内存溢出或编码失败
    if (!yuv_buffer_ || yuv_buffer_size_ < width * height * 3 / 2) {
        fprintf(stderr, "YUV buffer not allocated or too small\n");
        return -1;
    }

    // 使用 RGA 做 BGR -> YUV420P 转换（硬件加速）
    // wstride/hstride 为像素 stride；BGR 每像素 3 字节，故 wstride_pix = byte_stride/3
    int wstride_pix = (bgr_stride > 0) ? (bgr_stride / 3) : width;
    int hstride_pix = height;

    // 注意：RGA 的输入输出缓冲区需要满足特定的对齐和内存类型要求，使用虚拟地址包装函数时要确保地址可访问且符合 DMA 访问限制；
    // 如果 RGA 转换失败，可能是由于地址问题、驱动版本不兼容或其他硬件限制，此时会回退到 OpenCV 软件转换
    rga_buffer_t src_buf = wrapbuffer_virtualaddr((void*)bgr_data, width, height, wstride_pix, hstride_pix, RK_FORMAT_BGR_888);
    rga_buffer_t dst_buf = wrapbuffer_fd(yuv_dma_fd_, width, height, width, height, RK_FORMAT_YCbCr_420_P);
    // RGA 转换函数，参数包括源缓冲区、目标缓冲区、像素格式和颜色空间转换选项；如果转换成功，YUV 数据将存储在 yuv_buffer_ 中；
    // 如果转换失败，函数会返回错误码，此时会尝试使用 OpenCV 进行


    IM_STATUS status = imcvtcolor(src_buf, dst_buf,
                                  RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_P,
                                  IM_COLOR_SPACE_DEFAULT);
    if (status != IM_STATUS_SUCCESS) {
        // RGA 失败时回退到 OpenCV 软件转换（常见原因：虚拟地址 DMA 限制、驱动版本等）
        static int fallback_count = 0;
        // 仅在前几次失败时打印警告，避免日志过多；如果 RGA 转换失败，可能会导致性能下降，但至少能保证功能正常
        if (fallback_count++ < 3) {
            fprintf(stderr, "RGA color conversion failed (%d), using OpenCV fallback\n", (int)status);
        }
        // OpenCV 转换，首先创建一个 Mat 对象包装 BGR 数据，注意行跨度（stride）可能不等于宽度 * 3；
        int row_stride = (bgr_stride > 0) ? bgr_stride : (width * 3);
        // 如果输入数据不是连续的（如有行填充），则需要复制到一个连续的 Mat 中；如果输入数据已经连续，则直接使用原始数据指针；
        // OpenCV 的 cvtColor 函数要求输入数据连续，否则会抛出异常或产生错误的结果
        cv::Mat bgr(height, width, CV_8UC3, bgr_data, row_stride);
        if (!bgr.isContinuous()) {
            bgr = bgr.clone();
        }
        // 创建一个 Mat 对象包装 YUV 缓冲区，注意 YUV420P 格式的尺寸为宽高的 1.5 倍；如果 YUV 缓冲区未正确分配或大小不足，可能会导致内存访问错误
        cv::Mat yuv(height * 3 / 2, width, CV_8UC1, yuv_buffer_);
        // 使用 OpenCV 进行颜色空间转换，参数包括输入图像、输出图像和转换代码；如果转换成功，YUV 数据将存储在 yuv_buffer_ 中；如果转换失败，函数会抛出异常，此时编码器将无法正常工作
        cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV_I420);
    }

    int yuv_size = width * height * 3 / 2;
    // 调用编码函数进行编码，参数包括 YUV 数据指针、数据大小、输出数据包指针和输出数据包大小指针；
    // 如果编码成功，编码后的数据将存储在 packet_data 中，大小存储在 packet_size 中；
    return Encode(yuv_buffer_, yuv_size, packet_data, packet_size);
}

// 编码函数，输入为 YUV 数据和尺寸信息，输出为编码后的数据包和大小；函数内部会将 YUV 数据传递给 MPP 编码器进行编码，并轮询获取编码输出；
int MppEncoder::Encode(uint8_t* yuv_data, int yuv_size, uint8_t* packet_data, int* packet_size) {
    if (!initialized_) {
        fprintf(stderr, "Encoder not initialized\n");
        return -1;
    }

    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    MppBuffer buffer = NULL;
    // 创建一个新的 MPP 帧对象，用于存储输入的 YUV 数据；如果帧对象创建失败，编码器将无法接受输入数据
    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_frame_init failed\n");
        return -1;
    }
    // 从输入缓冲区组获取一个缓冲区，用于存储 YUV 数据；缓冲区大小通常为宽高的 1.5 倍（YUV420P 格式）；
    int buf_size = width_ * height_ * 3 / 2;
    ret = mpp_buffer_get(frm_grp_, &buffer, buf_size);
    if (ret != MPP_OK) {
        fprintf(stderr, "failed to get buffer for input frame ret %d\n", ret);
        mpp_frame_deinit(&frame);
        return -1;
    }
    // 将 YUV 数据复制到缓冲区，获取缓冲区的虚拟地址并进行内存复制；如果缓冲区指针无效或复制失败，编码器将无法正确处理输入数据
    void* buf_ptr = mpp_buffer_get_ptr(buffer);
    if (!buf_ptr) {
        fprintf(stderr, "failed to get buffer pointer\n");
        mpp_buffer_put(buffer);
        mpp_frame_deinit(&frame);
        return -1;
    }
    // 这个 memcpy 操作是将转换后的 YUV 数据复制到 MPP 编码器使用的缓冲区中，确保编码器能够正确访问输入数据；
    memcpy(buf_ptr, yuv_data, yuv_size);
    // 设置帧属性，包括宽高、像素格式、缓冲区和结束标志；这些属性告诉编码器如何处理输入数据，如果设置不正确可能会导致编码失败或输出错误的结果
    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420P);
    mpp_frame_set_buffer(frame, buffer);
    mpp_frame_set_eos(frame, 0);
    // 将帧对象传递给编码器进行编码，函数会将帧数据送入编码器的输入队列；
    // 如果编码器接受帧数据失败，可能是由于内部错误、资源不足或其他问题，此时编码器将无法正常工作
    ret = mpp_mpi_->encode_put_frame(mpp_ctx_, frame);
    if (ret != MPP_OK) {
        fprintf(stderr, "encode_put_frame failed ret %d\n", ret);
        // 这个错误处理路径确保在编码失败时正确释放资源，避免内存泄漏；
        mpp_buffer_put(buffer);
        // 帧对象在 encode_put_frame 之后不再需要，应该及时释放；如果不释放可能会导致内存泄漏或资源占用过多
        mpp_frame_deinit(&frame);
        return -1;
    }

    // 轮询获取编码输出，避免帧在编码器内堆积导致后续卡顿,
    const int max_retries = 50;
    // 循环尝试获取编码输出数据包，最多尝试 max_retries 次，每次间隔 1 毫秒；
    // 如果在 max_retries 次尝试后仍未获取到数据包，可能是编码器内部处理过慢或出现了其他问题，此时函数将返回错误码
    for (int r = 0; r < max_retries; r++) {
        ret = mpp_mpi_->encode_get_packet(mpp_ctx_, &packet);
        if (ret == MPP_OK && packet) {
            break;
        }
        if (ret != MPP_ERR_TIMEOUT) {
            *packet_size = 0;
            mpp_buffer_put(buffer);
            mpp_frame_deinit(&frame);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 检查获取数据包的结果，如果失败或数据包无效，函数将返回错误码；如果成功，函数将继续处理编码输出数据包
    if (ret != MPP_OK || !packet) {
        *packet_size = 0;
        mpp_buffer_put(buffer);
        mpp_frame_deinit(&frame);
        return 0;
    }
    // 获取数据包的有效载荷和大小，数据包包含编码后的比特流；如果数据包无效或大小为零，函数将返回错误码
    void* pkt_data = mpp_packet_get_data(packet);
    size_t pkt_size = mpp_packet_get_length(packet);

    // 检查数据包指针非空 且 数据包长度大于0
if (pkt_data && pkt_size > 0) {
    // 检查输出缓冲区指针非空 且 输出缓冲区大小 >= 数据包实际大小
    if (packet_data && *packet_size >= (int)pkt_size) {
        // 将数据包数据拷贝到输出缓冲区
        memcpy(packet_data, pkt_data, pkt_size);
        // 更新输出大小为实际拷贝的数据包长度
        *packet_size = pkt_size;
    } else {
        // 输出缓冲区无效或空间不足，设置输出大小为0
        *packet_size = 0;
        // 释放 MPP 数据包资源
        mpp_packet_deinit(&packet);
        // 释放 MPP 缓冲区资源
        mpp_buffer_put(buffer);
        // 释放 MPP 帧资源
        mpp_frame_deinit(&frame);
        // 返回错误码 -1
        return -1;
    }
} else {
    // 数据包指针为空或长度为0，设置输出大小为0
    *packet_size = 0;
}

// 统一释放 MPP 数据包资源
mpp_packet_deinit(&packet);
// 统一释放 MPP 缓冲区资源
mpp_buffer_put(buffer);
// 统一释放 MPP 帧资源
mpp_frame_deinit(&frame);

// 正常执行完成，返回 0
return 0;
}

/**
 * @brief 获取编码器头部信息（SPS/PPS 等码流头）
 * @param header_data 输出：头部数据缓冲
 * @param header_size 输入：缓冲区大小 / 输出：实际头部大小
 * @return 成功0，失败-1
 */
int MppEncoder::GetHeader(uint8_t* header_data, int* header_size) {
    // 检查编码器是否初始化完成
    if (!initialized_) {
        // 打印未初始化错误信息
        fprintf(stderr, "Encoder not initialized\n");
        // 返回错误码 -1
        return -1;
    }

    // 定义 MPP 操作返回值，初始化为成功
    MPP_RET ret = MPP_OK;
    // 定义 MPP 数据包指针，初始化为空
    MppPacket packet = NULL;

    // 调用 MPP 接口获取编码器额外信息（头部信息）
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_EXTRA_INFO, &packet);
    // 检查接口调用失败 或 数据包为空
    if (ret != MPP_OK || !packet) {
        // 打印获取额外信息失败错误
        fprintf(stderr, "get extra info failed ret %d\n", ret);
        // 返回错误码 -1
        return -1;
    }

    // 获取数据包的数据指针
    void* pkt_data = mpp_packet_get_data(packet);
    // 获取数据包的实际长度
    size_t pkt_size = mpp_packet_get_length(packet);

    // 检查数据包数据非空 且 长度大于0
    if (pkt_data && pkt_size > 0) {
        // 检查输出头部缓冲区非空 且 缓冲区大小足够存放头部数据
        if (header_data && *header_size >= (int)pkt_size) {
            // 将头部数据拷贝到输出缓冲区
            memcpy(header_data, pkt_data, pkt_size);
            // 更新实际头部大小
            *header_size = pkt_size;
            // 设置返回值为成功
            ret = MPP_OK;
        } else {
            // 缓冲区不足，打印缓冲区太小错误
            fprintf(stderr, "Header buffer too small: %d < %zu\n", *header_size, pkt_size);
            // 设置输出头部大小为0
            *header_size = 0;
            // 设置返回值为失败
            ret = MPP_NOK;
        }
    } else {
        // 无有效头部数据，设置输出大小为0
        *header_size = 0;
        // 设置返回值为失败
        ret = MPP_NOK;
    }

    // 释放 MPP 数据包资源
    mpp_packet_deinit(&packet);
    // 根据返回值，成功返回0，失败返回-1
    return (ret == MPP_OK) ? 0 : -1;
}

/**
 * @brief 释放编码器所有资源
 */
void MppEncoder::Release() {
    if (enc_cfg_) {
        mpp_enc_cfg_deinit(enc_cfg_);
        enc_cfg_ = NULL;
    }
    if (frm_grp_) {
        mpp_buffer_group_put(frm_grp_);
        frm_grp_ = NULL;
    }
    if (pkt_grp_) {
        mpp_buffer_group_put(pkt_grp_);
        pkt_grp_ = NULL;
    }
    if (mpp_ctx_) {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
    }

    // 释放 dma-buf 资源
    if (yuv_dma_fd_ >= 0) {
        close(yuv_dma_fd_);
        yuv_dma_fd_ = -1;
    }
    if (yuv_buffer_) {
        munmap(yuv_buffer_, yuv_buffer_size_);
        yuv_buffer_ = NULL;
        yuv_buffer_size_ = 0;
    }

    initialized_ = false;
}

