/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

// 包含自定义解码头文件
#include "decode.h"
// 包含断言库，用于程序调试检查
#include <cassert>

// 简单的对齐宏：将数值 x 按 a 对齐（向上取整），常用于内存对齐
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

// 简单的内存分配函数：封装 calloc，分配 size 个 type 类型的内存并清零
#define mpp_calloc(type, size) (type*)calloc(size, sizeof(type))

// 简单的断言宏：封装系统断言，条件不满足时程序崩溃并提示
#define mpp_assert(condition) assert(condition)

// 定义全局编码器参数结构体实例
MpiEncTestData encoder_params;
// 定义全局指针，指向上面的编码器参数结构体
MpiEncTestData *encoder_params_ptr = &encoder_params;

/**
 * @brief 初始化编码器上下文参数
 * @param p_ptr 二级指针，用于返回分配的参数结构体
 * @param cmd 编码命令参数（宽、高、格式等）
 * @return MPP_RET 执行状态码
 */
MPP_RET test_ctx_init(MpiEncTestData **p_ptr, MpiEncTestCmd *cmd)
{
    // 初始化返回值为成功
    MPP_RET ret = MPP_OK;
    // 取出一级指针
    MpiEncTestData *p = *p_ptr;
    // 如果指针为空，说明未分配内存
    if (!p)
    {
        // 分配 1 个 MpiEncTestData 结构体大小的内存并清零
        p = mpp_calloc(MpiEncTestData, 1);
        // 分配失败判断
        if (!p)
        {
            // 打印错误信息
            printf("malloc failed\n");
            // 返回内存分配失败错误码
            return MPP_ERR_MALLOC;
        }
        // 将新分配的结构体地址赋值给输出指针
        *p_ptr = p;
    }
    // 初始化帧缓冲区指针为空
    p->frm_buf = NULL;
    // 从命令参数中设置宽度
    p->width = cmd->width;
    // 从命令参数中设置高度
    p->height = cmd->height;
    // 水平步长：宽度按 16 字节对齐（硬件编码要求内存对齐）
    p->hor_stride = MPP_ALIGN(cmd->width, 16);
    // 垂直步长：高度按 16 字节对齐
    p->ver_stride = MPP_ALIGN(cmd->height, 16);
    // 设置图像格式（如 YUV420）
    p->fmt = cmd->format;
    // 设置编码类型（如 H.264）
    p->type = cmd->type;
    // 设置编码总帧数
    p->num_frames = cmd->num_frames;
    // 判断是否为 YUV420 格式
    if (p->fmt == MPP_FMT_YUV420P)
    {
        // YUV420 单帧大小 = 宽 * 高 * 1.5
        p->frame_size = p->hor_stride * p->ver_stride * 3 / 2;
    }
    else
    {
        // 不支持的格式打印错误
        printf("传入类型错误\n");
    }

    // 初始化码流包大小（预估值）
    p->packet_size = p->width * p->height;
    // 返回执行结果
    return ret;
}

/**
 * @brief 设置编码器各项参数
 * @param p 编码器参数结构体
 * @return MPP_RET 执行状态码
 */
MPP_RET test_mpp_setup(MpiEncTestData *p)
{
    // 定义返回值
    MPP_RET ret;
    // 定义 MPP API 接口指针
    MppApi *mpi;
    // 定义 MPP 上下文
    MppCtx ctx;
    // 定义编码协议配置结构体（H.264 等）
    MppEncCodecCfg *codec_cfg;
    // 定义输入预处理配置结构体
    MppEncPrepCfg *prep_cfg;
    // 定义码率控制配置结构体
    MppEncRcCfg *rc_cfg;
    // 从参数中获取 MPP API 指针
    mpi = p->mpi;
    // 从参数中获取 MPP 上下文
    ctx = p->ctx;
    // 取结构体地址，方便操作
    codec_cfg = &p->condec_cfg;
    prep_cfg = &p->prep_cfg;
    rc_cfg = &p->rc_cfg;
    // 设置帧率 30
    p->fps = 30;
    // 设置 GOP 关键帧间隔 30
    p->gop = 30;
    // 设置码率 4Mbps
    p->bps = 4096 * 1024;

    // ===================== 1. 输入控制配置 =====================
    // 标记需要修改：输入尺寸、旋转、格式
    prep_cfg->change = MPP_ENC_PREP_CFG_CHANGE_INPUT | MPP_ENC_PREP_CFG_CHANGE_ROTATION | MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    // 设置输入宽度
    prep_cfg->width = p->width;
    // 设置输入高度
    prep_cfg->height = p->height;
    // 设置水平对齐步长
    prep_cfg->hor_stride = p->hor_stride;
    // 设置垂直对齐步长
    prep_cfg->ver_stride = p->ver_stride;
    // 设置输入格式
    prep_cfg->format = p->fmt;
    // 设置旋转角度：0 度
    prep_cfg->rotation = MPP_ENC_ROT_0;
    // 调用 MPP 控制接口设置预处理参数
    ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    // 判断是否设置失败
    if (ret)
    {
        printf("mpi control enc set prep cfg failed ret %d\n", ret);
        return ret;
    }

    // ===================== 2. 码率控制配置 =====================
    // 标记更新所有码率参数
    rc_cfg->change = MPP_ENC_RC_CFG_CHANGE_ALL;
    // 码率模式：可变码率 VBR
    rc_cfg->rc_mode = MPP_ENC_RC_MODE_VBR;
    // 质量模式：固定 QP 编码
    rc_cfg->quality = MPP_ENC_RC_QUALITY_CQP;

    // 输入帧率配置：固定 30fps
    rc_cfg->fps_in_flex = 0;
    rc_cfg->fps_in_num = 30;
    rc_cfg->fps_in_denorm = 1;
    // 输出帧率与输入一致
    rc_cfg->fps_out_flex = 0;
    rc_cfg->fps_out_num = 30;
    rc_cfg->fps_out_denorm = 1;

    // GOP 关键帧间隔
    rc_cfg->gop = 30;
    // 不跳帧
    rc_cfg->skip_cnt = 0;

    // CQP 模式必须设置 QP 范围（0~51）
    rc_cfg->qp_min = 20;
    rc_cfg->qp_max = 51;

    // VBR 模式下的码率范围
    rc_cfg->bps_target = p->bps;
    rc_cfg->bps_max = p->bps * 2;
    rc_cfg->bps_min = p->bps / 2;

    // 设置码率控制参数
    ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret != MPP_OK)
    {
        printf("MPP_ENC_SET_RC_CFG failed! ret=%d\n", ret);
        return ret;
    }

    // ===================== 3. 协议控制配置（H.264） =====================
    // 设置编码格式 H.264
    codec_cfg->coding = p->type;
    // 设置 H.264 配置文件：Baseline 无 B 帧
    codec_cfg->h264.profile = 66;
    // 设置级别：31 对应 720p@30fps
    codec_cfg->h264.level = 31;
    // 熵编码：1=CABAC 高级编码
    codec_cfg->h264.entropy_coding_mode = 1;
    // CABAC 初始化参数
    codec_cfg->h264.cabac_init_idc = 0;
    // 设置编码协议参数
    ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret)
    {
        printf("mpi control enc set codec cfg failed ret %d\n", ret);
        return ret;
    }

    // ===================== 4. SEI 信息配置 =====================
    // 关闭 SEI 信息
    p->sei_mode = MPP_ENC_SEI_MODE_DISABLE;
    ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &p->sei_mode);
    if (ret)
    {
        printf("mpi control enc set sei cfg failed ret %d\n", ret);
        return ret;
    }

    // 设置头信息模式：仅 IDR 帧输出 SPS/PPS
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_DEFAULT;
    ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret)
    {
        printf("mpi control enc set header mode failed ret %d\n", ret);
        return ret;
    }
    // 返回成功
    return ret;
}

/**
 * @brief 将 OpenCV Mat YUV 数据拷贝到 MPP 硬件缓冲区
 * @param buf 目标 MPP 缓冲区
 * @param yuvImg 输入 YUV 图像
 * @param width 宽
 * @param height 高
 * @return int 0成功，异常抛出
 */
int read_yuv_buffer(RK_U8 *buf, cv::Mat &yuvImg, RK_U32 width, RK_U32 height)
{
    // Y 分量大小 = 宽 * 高
    size_t y_size = width * height;
    // U/V 分量各占 1/4
    size_t uv_size = y_size / 4;
    // YUV420 总大小
    size_t total_needed = y_size + 2 * uv_size;

    // 计算对齐后的缓冲区大小
    size_t buf_size = MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16) * 3 / 2;
    // 检查目标缓冲区是否足够
    if (buf_size < total_needed)
    {
        throw std::runtime_error("Target buffer too small");
    }

    // 检查源 Mat 数据是否完整
    if (yuvImg.total() * yuvImg.elemSize() < total_needed)
    {
        throw std::runtime_error("Source YUV data incomplete");
    }

    // ===================== 按格式拷贝数据 =====================
    // Y 分量起始地址
    RK_U8 *buf_y = buf;
    // U 分量起始地址（Y 之后）
    RK_U8 *buf_u = buf + MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16);
    // V 分量起始地址（U 之后）
    RK_U8 *buf_v = buf_u + MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16) / 4;

    // Mat 数据指针
    const RK_U8 *src = yuvImg.data;
    // 拷贝 Y
    memcpy(buf_y, src, y_size);
    // 拷贝 U
    memcpy(buf_u, src + y_size, uv_size);
    // 拷贝 V
    memcpy(buf_v, src + y_size + uv_size, uv_size);
    // 返回成功
    return 0;
}

/**
 * @brief 执行 MPP 硬编码：输入 YUV 输出 H.264
 * @param yuvImg 输入 YUV 图像
 * @param mpi MPP API
 * @param ctx MPP 上下文
 * @param H264_buf 输出编码数据
 * @param length 输出长度
 * @return MPP_RET
 */
MPP_RET test_mpp_run_yuv(Mat yuvImg, MppApi *mpi, MppCtx &ctx,
                         unsigned char *&H264_buf, int &length)
{
    // 参数合法性检查
    if (!mpi || !encoder_params_ptr || yuvImg.empty())
    {
        printf("Invalid parameters\n");
        return MPP_ERR_NULL_PTR;
    }

    // 获取全局参数指针
    MpiEncTestData *p = encoder_params_ptr;
    MPP_RET ret = MPP_OK;
    // 定义输入帧结构体
    MppFrame frame = NULL;
    // 定义输出码流包结构体
    MppPacket packet = NULL;

    // 记录编码开始时间
    auto encode_start = std::chrono::high_resolution_clock::now();

    // 1. 获取 MPP 缓冲区指针
    void *buf = mpp_buffer_get_ptr(p->frm_buf);
    if (!buf)
    {
        printf("Failed to get frame buffer pointer\n");
        return MPP_ERR_NULL_PTR;
    }

    // 2. 将 YUV 数据拷贝到硬件缓冲区
    if (read_yuv_buffer((RK_U8 *)buf, yuvImg, p->width, p->height) != 0)
    {
        printf("Failed to read yuv buffer\n");
        return MPP_ERR_VALUE;
    }

    // 3. 初始化 MPP 帧
    ret = mpp_frame_init(&frame);
    if (ret)
    {
        printf("mpp_frame_init failed: %d\n", ret);
        goto FAIL;
    }

    // 设置帧宽度
    mpp_frame_set_width(frame, p->width);
    // 设置帧高度
    mpp_frame_set_height(frame, p->height);
    // 设置水平步长
    mpp_frame_set_hor_stride(frame, p->hor_stride);
    // 设置垂直步长
    mpp_frame_set_ver_stride(frame, p->ver_stride);
    // 设置格式
    mpp_frame_set_fmt(frame, p->fmt);
    // 绑定缓冲区
    mpp_frame_set_buffer(frame, p->frm_buf);
    // 设置结束标志
    mpp_frame_set_eos(frame, p->frm_eos);

    // 4. 将帧送入编码器
    ret = mpi->encode_put_frame(ctx, frame);
    if (ret)
    {
        printf("encode_put_frame failed: %d\n", ret);
        goto FAIL;
    }

    // 5. 从编码器获取编码后码流
    ret = mpi->encode_get_packet(ctx, &packet);
    if (ret)
    {
        printf("encode_get_packet failed: %d\n", ret);
        goto FAIL;
    }

    // 如果获取到有效码流包
    if (packet)
    {
        // 6. 获取码流数据指针和长度
        void *ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);

        // 检查数据有效性
        if (!ptr || len <= 0)
        {
            printf("Invalid packet data\n");
            ret = MPP_ERR_VALUE;
            goto FAIL;
        }

        // 7. 重新分配输出缓冲区
        if (H264_buf)
        {
            free(H264_buf);
            H264_buf = nullptr;
        }
        // 分配内存
        H264_buf = (unsigned char *)malloc(len);
        if (!H264_buf)
        {
            printf("Failed to allocate output buffer\n");
            ret = MPP_ERR_MALLOC;
            goto FAIL;
        }

        // 拷贝编码数据
        memcpy(H264_buf, ptr, len);
        // 输出长度
        length = len;

        // 8. 编码帧数统计
        static int frame_count = 0;
        frame_count++;
        // 计算编码耗时
        auto encode_end = std::chrono::high_resolution_clock::now();
        auto encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start);

        // 9. 更新统计信息
        p->pkt_eos = mpp_packet_get_eos(packet);
        p->stream_size += len;
        p->frame_count++;

        // 判断是否为最后一帧
        if (p->pkt_eos)
        {
            printf("Found last packet\n");
            mpp_assert(p->frm_eos);
        }
    }

FAIL:
    // 10. 释放资源
    if (frame) mpp_frame_deinit(&frame);
    if (packet) mpp_packet_deinit(&packet);

    return ret;
}

/**
 * @brief 编码器初始化（创建 MPP 上下文、设置参数）
 * @param p 编码器参数
 * @param width 宽
 * @param height 高
 * @return 初始化后的参数指针
 */
MpiEncTestData *test_mpp_run_yuv_init(MpiEncTestData *p, int width, int height)
{
    printf("init \n");

    MPP_RET ret;
    // 定义命令参数
    MpiEncTestCmd cmd;
    cmd.width = width;
    cmd.height = height;
    // 编码格式 H.264
    cmd.type = MPP_VIDEO_CodingAVC;
    // 输入格式 YUV420
    cmd.format = MPP_FMT_YUV420P;
    // 帧数 0（实时编码）
    cmd.num_frames = 0;

    // 初始化参数结构体
    ret = test_ctx_init(&p, &cmd);
    if (ret)
    {
        printf("test data init failed ret %d\n", ret);
        return p;
    }

    // 申请一帧图像大小的硬件缓冲区
    ret = mpp_buffer_get(NULL, &p->frm_buf, p->frame_size);
    if (ret)
    {
        printf("failed to get buffer for input frame ret %d\n", ret);
        return p;
    }

    // 创建 MPP 上下文和 API
    ret = mpp_create(&p->ctx, &p->mpi);
    if (ret)
    {
        printf("mpp_create failed ret %d\n", ret);
        return p;
    }
    printf("ctx = %p, mpi = %p\n", p->ctx, p->mpi);

    // 初始化为编码器模式
    ret = mpp_init(p->ctx, MPP_CTX_ENC, p->type);
    if (ret)
    {
        printf("mpp_init failed ret %d\n", ret);
        return p;
    }

    // 设置编码参数
    ret = test_mpp_setup(p);
    if (ret)
    {
        printf("test mpp setup failed ret %d\n", ret);
        return p;
    }

    // 全局保存 API 和上下文
    mpi = p->mpi;
    ctx = p->ctx;

    return p;
}

// 单例模式：获取编码器实例
MppEncoder &MppEncoder::instance()
{
    static MppEncoder encoder;
    return encoder;
}

/**
 * @brief 编码接口：YUV -> H.264（首帧自动初始化）
 * @param width 宽
 * @param height 高
 * @param yuv_frame 输入 YUV
 * @param encode_buf 输出 H.264
 * @param encode_length 输出长度
 * @return MPP_RET
 */
MPP_RET MppEncoder::encode(int width, int height, Mat yuv_frame,
                           char *(&encode_buf), size_t &encode_length)
{
    // 定义输出缓冲区
    unsigned char *H264_buf = NULL;
    int H264_buf_length = 0;
    MPP_RET ret = MPP_OK;

    // 判断是否为第一帧
    if (first_frame_flg.load())
    {
        printf("first_packet! \n");
        // 第一帧：初始化编码器
        encoder_params_ptr = test_mpp_run_yuv_init(encoder_params_ptr, width, height);
        // 编码第一帧（包含 SPS/PPS）
        ret = test_mpp_run_yuv(yuv_frame, mpi, ctx, H264_buf, H264_buf_length);
        if (ret)
        {
            printf("test_mpp_run_yuv first frame failed: %d\n", ret);
        }
        else
        {
            // 拷贝数据到输出
            memcpy(encode_buf, H264_buf, H264_buf_length);
            encode_length = static_cast<size_t>(H264_buf_length);
            // 清除首帧标志
            first_frame_flg.store(false);
        }
    }
    else
    {
        // 非首帧：直接编码
        ret = test_mpp_run_yuv(yuv_frame, mpi, ctx, H264_buf, H264_buf_length);
        if (ret)
        {
            printf("test_mpp_run_yuv frame failed: %d\n", ret);
        }
        else
        {
            memcpy(encode_buf, H264_buf, H264_buf_length);
            encode_length = static_cast<size_t>(H264_buf_length);
        }
    }

    // 释放临时缓冲区
    if (H264_buf)
    {
        delete H264_buf;
        H264_buf = nullptr;
    }

    return ret;
}

// 兼容旧接口：直接调用单例编码
void YuvtoH264(int width, int height, Mat yuv_frame, char *(&encode_buf), size_t &encode_length)
{
    (void)MppEncoder::instance().encode(width, height, yuv_frame, encode_buf, encode_length);
}

/**
 * @brief 分割 H.264 码流为单个 NALU 单元
 * @param packet 码流数据
 * @param packetSize 长度
 * @return 所有 NALU 集合
 */
std::vector<H264Frame> splitNalus(const char *packet, int packetSize)
{
    // 存储 NALU 列表
    std::vector<H264Frame> nalus;
    // 遍历位置
    size_t pos = 0;
    // 起始码长度
    size_t m_startCodeSize = 0;

    // 遍历整个码流
    while (pos < packetSize)
    {
        // 剩余数据不足 3 字节，退出
        if (pos + 3 > packetSize) break;
        size_t startCodeSize = 0;

        // 检查 3 字节起始码 00 00 01
        if (packet[pos] == 0x00 && packet[pos + 1] == 0x00 && packet[pos + 2] == 0x01)
        {
            startCodeSize = 3;
            m_startCodeSize = startCodeSize;
        }
        // 检查 4 字节起始码 00 00 00 01
        else if (pos + 4 <= packetSize &&
                 packet[pos] == 0x00 && packet[pos + 1] == 0x00 &&
                 packet[pos + 2] == 0x00 && packet[pos + 3] == 0x01)
        {
            startCodeSize = 4;
            m_startCodeSize = startCodeSize;
        }
        else
        {
            // 未找到，位置+1
            pos++;
            continue;
        }

        // 记录 NALU 起始位置
        size_t naluStart = pos;
        // 跳过起始码
        pos += startCodeSize;

        // 查找下一个起始码，确定当前 NALU 结束
        while (pos < packetSize)
        {
            if (pos + 3 <= packetSize &&
                packet[pos] == 0x00 && packet[pos + 1] == 0x00 &&
                packet[pos + 2] == 0x01) break;
            if (pos + 4 <= packetSize &&
                packet[pos] == 0x00 && packet[pos + 1] == 0x00 &&
                packet[pos + 2] == 0x00 && packet[pos + 3] == 0x01) break;
            pos++;
        }

        // 计算 NALU 大小
        size_t naluSize = pos - naluStart;
        if (naluSize > 0)
        {
            H264Frame nalu;
            // 分配空间
            nalu.data.resize(naluSize);
            // 拷贝数据
            memcpy(nalu.data.data(), packet + naluStart, naluSize);
            // 记录起始码长度
            nalu.startSize = m_startCodeSize;
            // 加入列表
            nalus.push_back(std::move(nalu));
        }
    }
    return nalus;
}