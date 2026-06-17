/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * 版权声明：2025-04-01 HeXiaotian 保留所有权利
 *
 * This source code is licensed for learning and research purposes only.
 * 本源代码仅许可用于学习和研究目的
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 * 未经作者事先书面许可，严禁商业使用、再分发、转售以及创建衍生作品
 */

#ifndef MPP_ENCODER_H                     // 头文件保护宏：如果未定义MPP_ENCODER_H，则定义它，防止重复包含
#define MPP_ENCODER_H                     // 定义宏，表示头文件已被包含

#include <stdint.h>                       // 包含标准整数类型定义（如uint8_t, uint32_t等）
#include "rk_mpi.h"                       // 包含Rockchip MPP MPI接口头文件
#include "rk_venc_cfg.h"                  // 包含Rockchip视频编码配置结构体头文件



class MppEncoder {                        // 定义MppEncoder类，用于MPP硬件编码器封装
public:
    MppEncoder();                         // 构造函数，初始化成员变量
    ~MppEncoder();                        // 析构函数，释放资源

    // 初始化编码器
    // width/height: 编码分辨率
    // fps:          帧率
    // bitrate:      码率 (bps)
    // codec_type:   264(H.264) / 265(H.265)
    int Init(int width, int height, int fps, int bitrate, int codec_type = 264);
    // 初始化编码器，参数依次为宽度、高度、帧率、码率、编码类型（默认H.264）

    // 直接编码一帧 YUV420P 数据
    int Encode(uint8_t* yuv_data, int yuv_size, uint8_t* packet_data, int* packet_size);
    // 编码YUV420P格式的一帧数据
    // yuv_data: YUV数据指针
    // yuv_size: YUV数据大小
    // packet_data: 输出编码数据缓冲区
    // packet_size: 输入缓冲区大小，输出实际编码数据大小

    // 编码一帧 BGR 图像（使用 RGA 做 BGR->YUV420P，失败时回退 OpenCV）
    // bgr_stride: 可选，BGR 行字节步长；0 表示 width*3
    int EncodeFrame(uint8_t* bgr_data, int width, int height, uint8_t* packet_data, int* packet_size, int bgr_stride = 0);
    // 编码BGR格式的一帧图像，自动转换为YUV420P后进行编码
    // bgr_data: BGR图像数据指针
    // width: 图像宽度
    // height: 图像高度
    // packet_data: 输出编码数据缓冲区
    // packet_size: 输入缓冲区大小，输出实际编码数据大小
    // bgr_stride: BGR图像的行步长（字节数），0表示自动按width*3计算

    // 获取 SPS / PPS 等头信息（可用于 FFmpeg extradata）
    int GetHeader(uint8_t* header_data, int* header_size);
    // 获取编码器的序列参数集（SPS）和图像参数集（PPS）等头部信息
    // header_data: 输出头部数据缓冲区
    // header_size: 输入缓冲区大小，输出实际头部数据大小

    // 释放资源
    void Release();
    // 主动释放编码器占用的所有资源

private:
    MppCtx          mpp_ctx_;          // MPP上下文句柄
    MppApi*         mpp_mpi_;          // MPP API接口指针
    MppEncCfg       enc_cfg_;          // 编码器配置结构体指针
    MppFrame        frame_;            // MPP帧结构，用于存放输入帧数据
    MppPacket       packet_;           // MPP包结构，用于存放输出编码数据
    MppBufferGroup  frm_grp_;          // 帧缓冲区组，管理帧内存
    MppBufferGroup  pkt_grp_;          // 包缓冲区组，管理包内存

    int             width_;            // 编码分辨率宽度
    int             height_;           // 编码分辨率高度
    int             fps_;              // 帧率
    int             bitrate_;          // 码率（bps）
    MppCodingType   mpp_type_;         // MPP编码类型（如MPP_VIDEO_CodingAVC或MPP_VIDEO_CodingHEVC）
    bool            initialized_;      // 初始化标志，指示编码器是否已成功初始化

    // 预分配的 YUV 缓冲区，避免每帧 malloc/free
    uint8_t*        yuv_buffer_;       // YUV420P数据缓冲区指针
    int             yuv_buffer_size_;  // YUV缓冲区大小（字节）
    int yuv_dma_fd_;   // dma-buf 文件描述符
};

#endif // MPP_ENCODER_H                  // 结束头文件保护宏