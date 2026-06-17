

/*
 * ============================================================================
 * 文件：mpp_decoder.cpp
 * 描述：Rockchip MPP 硬件解码器实现
 * 功能：封装 MPP 解码器，支持 H.264/H.265 解码，通过回调返回 YUV 帧
 * ============================================================================
 */

#include <stdio.h>                 // 标准输入输出头文件
#include <sys/time.h>              // 系统时间相关
#include <unistd.h>                // 提供 usleep 函数 ，微秒级
#include <iostream>                // C++标准输入输出流
#include "mpp_decoder.h"           // 自定义编解码头文件包含 MppDecoder 类，和MPP 结构体

// 构造函数，初始化成员变量，当前为空，后续 Init 中初始化
MppDecoder::MppDecoder() {}

// 析构函数，释放 MPP 解码器占用的所有资源
MppDecoder::~MppDecoder()
{
    // 如果 loop_data.packet 不为空，说明有未释放的 MPP 数据包
    if (loop_data.packet)
    {
        // 专用api 函数，释放数据包资源
        mpp_packet_deinit(&loop_data.packet);
        // 置空指针，防止资源二次释放，导致程序崩溃
        loop_data.packet = NULL;
    }
    // 当前 frame 不为空，说明有未释放的解码帧
    if (frame)
    {
        // 专用 api 函数，释放解码帧资源
        mpp_frame_deinit(&frame);
        // 置空指针，防止资源二次释放，导致程序崩溃
        frame = NULL;
    }
    // 如果 mpp_ctx 不为空，说明 MPP 上下文已经创建
    if (mpp_ctx)
    {
        // 专用 api 函数，销毁 MPP 上下文
        mpp_destroy(mpp_ctx);
        // 置空指针，防止资源二次释放，导致程序崩溃
        mpp_ctx = NULL;
    }
    // 如果 loop_data.frm_grp 不为空，说明有帧缓存组
    if (loop_data.frm_grp)
    {
        // 专用 api 函数，释放缓冲区组引用
        mpp_buffer_group_put(loop_data.frm_grp);
        // 置空指针，防止资源二次释放，导致程序崩溃
        loop_data.frm_grp = NULL;
    }
}

// 初始化解码器
// 参数：video_type - 264 或 265
//fps               - 帧率（暂未使用）
//userdata          - 用户数据（传给回调）
//id                - 流 ID
int MppDecoder::Init(int video_type, int fps, void *userdata,int id)
{
    // 保存流 id
    this->id=id; 
    // MPP 返回码
    MPP_RET ret = MPP_OK;
    // 用户数据指针，回调时传回
    this->userdata = userdata;
    // 保存帧率
    this->fps = fps;
    // 初始化上一帧时间戳
    this->last_frame_time_ms = 0;
    // 根据视频类型，标记 MPP 标识
    if (video_type == 264)
    {
        // H.264
        mpp_type = MPP_VIDEO_CodingAVC;
    }
    else if (video_type == 265)
    {
        // H.265
        mpp_type = MPP_VIDEO_CodingHEVC;
    }
    else
    {
        // printf("unsupport video_type %d", video_type);
        return -1;
    }
    // 初始化 Loop_data 结构体全为 0 
    memset(&loop_data, 0, sizeof(loop_data));
                                              // printf("mpi_dec_test decoder test start mpp_type %d ", mpp_type);
    // 解码器配置结构
    MppDecCfg cfg = NULL;
    // 临时 MPP 上下文
    MppCtx mpp_ctx = NULL;
    // MPP 接口指针
    mpp_mpi = NULL;

    // 创建 MPP 上下文 mpp_ctx 和 MPP 接口 mpp_mpi
    ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (MPP_OK != ret)
    {
        // printf("mpp_create failed ");
        return 0;
    }

    // 初始化解码器，指定解码器为 MPP_CTX_DEC ,和编码类型 mpp_type
    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    if (ret)
    {
        // printf("%p mpp_init failed ", mpp_ctx);
        return -1;
    }

    // 初始化解码器配置结构
    mpp_dec_cfg_init(&cfg);
    // 从解码器获取当前结构体配置
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret)
    {
        // printf("%p failed to get decoder cfg ret %d ", mpp_ctx, ret);
        return -1;
    }

    // 设置 split_parse ,
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret)
    {
        // printf("%p failed to set split_parse ret %d ", mpp_ctx, ret);
        return -1;
    }
    // 将修改后的配置写回解码器
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    if (ret)
    {
        // printf("%p failed to set cfg %p ret %d ", mpp_ctx, cfg, ret);
        return -1;
    }
    // 释放配置结构
    mpp_dec_cfg_deinit(cfg);
    // 将创建的上下文，和接口保存到 Loop_data 中，提供给后续解码使用
    loop_data.ctx = mpp_ctx;
    loop_data.mpi = mpp_mpi;
    loop_data.eos = 0;
    loop_data.frame = NULL;
    return 1;
}

// 重置解码器，清空内部缓存，准备从头开始解码
int MppDecoder::Reset()
{
    if (mpp_mpi != NULL)
    {
        // 专用 api MPP接口函数，重置接口，释放资源
        mpp_mpi->reset(mpp_ctx);
    }
    return 0;
}

// 设置回调函数，解码出的每一帧回调
int MppDecoder::SetCallback(MppDecoderFrameCallback callback)
{
    this->callback = callback;
    return 0;
}

// 解码数据包
// 参数：pkt_data - 压缩数据指针
// pkt_size        - 数据大小
// pkt_eos         - 是否为最后一个包（文件结束标志）
// 返回值类型为 bool True 成功解码输出一帧， False 失败
int MppDecoder::Decode(uint8_t *pkt_data, int pkt_size, int pkt_eos)
{
    // 指向解码循环数据包的指针
    MpiDecLoopData *data = &loop_data;
    // 标记当前数据帧是否已经送入解码器
    RK_U32 pkt_done = 0;
    // 错误信息
    RK_U32 err_info = 0;
    // MPP 返回标记状态码
    MPP_RET ret = MPP_OK;
    // 解码上下文
    MppCtx ctx = data->ctx;
    // MPP 接口
    MppApi *mpi = data->mpi;
    // 统计当前解码获得的帧数，初始为 0
    int got_frames = 0;
    // 如果初始 packet 结构体为空，初始化一个 MPP 数据包
    if (packet == NULL)
    {
        ret = mpp_packet_init(&packet, NULL, 0);
    }
    // 设置 packet 的数据指针和大小
    // 数据起始地址
    mpp_packet_set_data(packet, pkt_data);
    // 数据总大小
    mpp_packet_set_size(packet, pkt_size);
    // 记录当前读取位置
    mpp_packet_set_pos(packet, pkt_data);
    // 有效数据长度
    mpp_packet_set_length(packet, pkt_size);
    
    // 如果最后一个包，设置 EOS 标志
    if (pkt_eos)
        mpp_packet_set_eos(packet);

    // 主循环；将数据包送入解码器，并且尝试获取帧，直到包被消耗无新帧输出
    do
    {
        // 超时重试次数 5 次
        RK_S32 times = 5;
        
        // 判断当前没有数据包送入解码器，尝试送入
        if (!pkt_done)
        {
            //专用 api 函数，送包带解码器
            ret = mpi->decode_put_packet(ctx, packet);
            // 成功送入
            if (MPP_OK == ret)
                pkt_done = 1;
        }
        // 反复读取解码后输出的帧，直到没有新帧
        do
        {
            // 标记本次循环是否成功获取到了帧
            RK_S32 get_frm = 0;
            // 检测获取到的帧是否带有 EOS 标志
            RK_U32 frm_eos = 0;
        // 再次尝试
        try_again:
            // 从解码器获取帧
            ret = mpi->decode_get_frame(ctx, &frame);
                                                        // std::cout << "ret :" << ret << std::endl;
            // 如果获取超时，反复尝试若干次
            if (MPP_ERR_TIMEOUT == ret)
            {
                if (times > 0)
                {
                    times--;
                    // 休眠两秒，避免 CPU 空转，降低功耗，给 MPP 硬件反应时间
                    usleep(2000);
                    // 重新尝试
                    goto try_again;
                }
            }
            // 如果返回非 MPP_OK 且不是超时，则跳出内循环
            if (MPP_OK != ret)
            {
                // printf("decode_get_frame failed ret %d ", ret);
                break;
            }
            // 成功读取到一帧
            if (frame)
            {
                // 获取缓冲区大小
                RK_U32 buf_size = mpp_frame_get_buf_size(frame);
                // 检查是否发生信息变化(分辨率等)
                if (mpp_frame_get_info_change(frame))
                {
                    // 处理分辨率变化
                    // 如果缓冲区组，未创建，则创建一个 DRAM 缓冲组                    
                    if (NULL == data->frm_grp)
                    {
                        ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret)
                        {
                            // printf("%p get mpp buffer group failed ret %d ", ctx, ret);
                            break;
                        }
                        // 将缓冲组设置给解码器
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
                        if (ret)
                        {
                            // printf("%p set buffer group failed ret %d ", ctx, ret);
                            break;
                        }
                    }
                    else
                    {
                        // 已经存在的缓冲组需要清空，重新配置
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret)
                        {
                            // printf("%p clear buffer group failed ret %d ", ctx, ret);
                            break;
                        }
                    }
                    // 配置缓冲区，限制大小和数量
                    ret = mpp_buffer_group_limit_config(data->frm_grp, buf_size, 24);
                    if (ret)
                    {
                        // printf("%p limit buffer group failed ret %d ", ctx, ret);
                        break;
                    }
                    // 通知解码器信息变化处理完毕
                    ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret)
                    {
                        // printf("%p info change ready failed ret %d ", ctx, ret);
                        break;
                    }

                }
                else
                {
                    // err_info = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
                    // if (err_info) {
                    //     // printf("decoder_get_frame get err info:%d discard:%d. ", mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
                    // }

                    // 正常解码帧，获取各种属性
                    // 水平步长
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    // 垂直步长
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                    // 实际宽度
                    RK_U32 hor_width = mpp_frame_get_width(frame);
                    // 实际高度
                    RK_U32 ver_height = mpp_frame_get_height(frame);
                    // 显示时间戳
                    RK_S64 pts = mpp_frame_get_pts(frame);
                    // 解码时间戳
                    RK_S64 dts = mpp_frame_get_dts(frame);

                    // std::cout<<hor_width<<" "<<ver_height<<" "<<hor_stride<<" "<<ver_stride<<std::endl;
                    // // printf("decoder require buffer w:h [%d:%d] stride [%d:%d] buf_size %d pts=%lld dts=%lld ", hor_width, ver_height, hor_stride,
                    //      ver_stride, buf_size, pts, dts);

                    // 计数器加一
                    got_frames++;
                    // 如果回调函数已经设置，则调用回调函数，返回帧
                    if (callback != nullptr)
                    {
                        // 获取像素格式
                        MppFrameFormat format = mpp_frame_get_fmt(frame);
                        // 获取帧数据的地址指针
                        char *data_vir = (char *)mpp_buffer_get_ptr(mpp_frame_get_buffer(frame));
                        // 调用回调函数，传递用户数据的步长、高度、宽度、格式、文件描述符、数据指针、流 id
                        callback(this->userdata, hor_stride, ver_stride, hor_width, ver_height, format, 0, data_vir,this->id);
                    }
                }
                // 检查帧是否包含 EOS 标志
                frm_eos = mpp_frame_get_eos(frame);
                // 销毁 解码器当前的帧资源
                ret = mpp_frame_deinit(&frame);
                // 指针置空，防止二次释放。重新崩溃
                frame = NULL;
                // 标记本次循环成功获取到了帧
                get_frm = 1;
            }
             // 如果是最后一个包且已送入解码器，但还未收到 EOS 帧，则短暂等待后继续
            if (pkt_eos && pkt_done && !frm_eos)
            {
                // 休眠 1s
                usleep(1 * 1000);
                // 继续尝试获取帧
                continue;
            }
             // 如果当前帧是 EOS 帧，则结束内循环
            if (frm_eos)
            {
                // printf("found last frame ");
                // 退出
                break;
            }
            // 本次成功获取到了帧
            if (get_frm)
            // 持续尝试获取帧
                continue;
            // 退出循环
            break;
        } while (1);
        // 如果包已成功送入解码器，则退出外循环（等待包消耗完毕）
        if (pkt_done)
            break;
        // 包尚未送入解码器（可能缓冲区满），休眠 3 毫秒后重试
        usleep(3 * 1000);
    } while (1);
     // 释放 packet 资源（本次解码使用的 MPP 数据包）
    mpp_packet_deinit(&packet);
    // 返回是否至少获得了一帧
    return got_frames > 0;
}


