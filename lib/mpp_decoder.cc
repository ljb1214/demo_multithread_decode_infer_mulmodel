#include "mpp_decoder.h"    // 包含自定义的 MPP 解码器头文件，声明了 MppDecoder 类及相关结构
#include <cstring>          // 包含 C 标准库字符串函数，如 memset、memcpy 等
#include <iostream>         // 包含 C++ 输入输出流，用于错误信息输出（std::cerr）
#include <unistd.h>         // 包含 POSIX 系统调用，如 usleep 用于微秒级延时,是系统变成常用头文件

// MppDecoder 类的构造函数，用于初始化成员变量
MppDecoder::MppDecoder()
{
    // MPP 上下文句柄，初始化为空
    mpp_ctx = NULL;
    // MPP 接口函数表指针，初始化为空
    mpp_mpi = NULL;
    // MPP 数据包句柄，用于存放待解码的输入数据，初始为空
    packet = NULL;
    // MPP 帧句柄，用于存放解码输出的帧，初始为空
    frame = NULL;
    // 回调函数指针，用于将解码后的帧传递给上层，初始为空
    callback = nullptr;
    // 用户自定义数据指针，会作为回调函数的参数，初始为空
    userdata = nullptr;
    // 视频帧率，初始为 -1 表示未设置
    fps = -1;
     // 解码器实例 ID，用于多路解码时区分，初始为 0
    id = 0;
    // 上一帧解码完成的时间戳（毫秒），用于调整帧率对齐输入视频的帧率，初始为 0
    last_frame_time_ms = 0;
     // 编码类型，默认设置为 H.264 (AVC)
    mpp_type = MPP_VIDEO_CodingAVC;
    // 是否需要 MPP 内部拆分 NALU，1 表示启用（自动处理起始码）
    need_split = 1;
}

// MppDecoder 类的析构函数，释放所有资源
MppDecoder::~MppDecoder()
{
    // 如果 loop_data 中的 packet 不为空
    if (loop_data.packet)
    {
        // 释放 MPP 数据包资源
        mpp_packet_deinit(&loop_data.packet);
        // 将指针置空，防止野指针
        loop_data.packet = NULL;
    }
    // 如果当前 frame 句柄不为空
    if (frame)
    {
        // 销毁 MPP 上下文，释放相关资源
        mpp_frame_deinit(&frame);
        // 将指针置空，防止野指针
        frame = NULL;
    }
     // 如果 MPP 上下文句柄不为空
    if (mpp_ctx)
    {
        mpp_destroy(mpp_ctx);
        // 将指针置空，防止野指针
        mpp_ctx = NULL;
    }
    // 如果 loop_data 中的帧缓冲区组不为空
    if (loop_data.frm_grp)
    {
        // 释放缓冲区组引用，当引用计数为 0 时释放资源
        mpp_buffer_group_put(loop_data.frm_grp);
        // 将指针置空，防止野指针
        loop_data.frm_grp = NULL;
    }
}

// 初始化解码器，参数：video_type=264或265，fps=帧率，userdata=用户数据，id=实例ID
int MppDecoder::Init(int video_type, int fps, void *userdata, int id)
{
    // 保存实例 ID 到成员变量
    this->id = id;
    // 定义 MPP 返回码变量，初始为成功
    MPP_RET ret = MPP_OK;
     // 保存用户数据指针
    this->userdata = userdata;
    // 保存帧率
    this->fps = fps;
    // 重置上一帧时间戳为 0
    this->last_frame_time_ms = 0;
    // 如果视频类型是 264
    if (video_type == 264)
    {
        // 设置 MPP 编码类型为 H.264
        mpp_type = MPP_VIDEO_CodingAVC;
    }
    // 否则如果视频类型是 265
    else if (video_type == 265)
    {
         // 设置 MPP 编码类型为 H.265
        mpp_type = MPP_VIDEO_CodingHEVC;
    }
    else
    {
        // 输出错误信息到标准错误
        std::cerr << "Unsupported video_type: " << video_type << std::endl;
        return -1;
    }

    // 将 loop_data 结构体所有字节清零（loop_data 是 MpiDecLoopData 类型，用于解码循环的状态）
    memset(&loop_data, 0, sizeof(loop_data));
    // 定义 MPP 解码器配置句柄，初始为空
    MppDecCfg cfg = NULL;
    // 创建 MPP 上下文和 MPI 接口表，返回结果存入 ret
    ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (MPP_OK != ret)
    {
         // 输出错误信息
        std::cerr << "mpp_create failed" << std::endl;
        // 返回错误状态
        return -1;
    }
    
    // 初始化 MPP 上下文为解码器，使用指定的编码类型（H.264/H.265）
    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    // 如果初始化失败
    if (ret)
    {
        // 输出错误信息
        std::cerr << "mpp_init failed" << std::endl;
        // 返回错误状态
        return -1;
    }
    
    // 初始化解码器配置结构体，分配内存并设置默认值
    mpp_dec_cfg_init(&cfg);
    // 通过 MPI 控制接口获取当前解码器的配置，存入 cfg
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    // 如果设置失败
    if (ret)
    {
        // 输出错误信息及错误码
        std::cerr << "failed to get decoder cfg ret " << ret << std::endl;
        // 返回错误状态
        return -1;
    }

    // 设置解码器配置项 "base:split_parse" 为 need_split（1=启用内部拆分 NALU）
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    // 如果设置失败
    if (ret)
    {
        // 输出错误信息及错误码
        std::cerr << "failed to set split_parse ret " << ret << std::endl;
        // 返回错误状态
        return -1;
    }
    // 将修改后的配置设置回解码器
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    // 如果设置失败
    if (ret)
    {
        // 输出错误信息及错误码
        std::cerr << "failed to set cfg ret " << ret << std::endl;
        // 返回错误状态
        return -1;
    }
    
    // 释放解码器配置结构体资源
    mpp_dec_cfg_deinit(cfg);
     // 将 MPP 上下文保存到 loop_data 中
    loop_data.ctx = mpp_ctx;
    // 将 MPI 接口表保存到 loop_data 中
    loop_data.mpi = mpp_mpi;
    // 设置结束标志为 0（未结束）
    loop_data.eos = 0;
    // 清空 loop_data 中的帧句柄
    loop_data.frame = NULL;
    // 初始化成功，返回 0  
    return 0;
}

// 重置解码器状态
int MppDecoder::Reset()
{
    // 如果 MPI 接口表已初始化
    if (mpp_mpi != NULL)
    {
        // 调用 MPP 重置接口，清空解码器内部状态
        mpp_mpi->reset(mpp_ctx);
    }
    // 返回 0 表示成功
    return 0;
}

// 设置解码完成后的回调函数
int MppDecoder::SetCallback(MppDecoderFrameCallback callback)
{
    // 将传入的回调函数指针保存到成员变量
    this->callback = callback;
    // 返回 0 表示成功
    return 0;
}

// 解码一帧或多帧数据，参数：pkt_data=输入码流数据，pkt_size=数据大小，pkt_eos=是否最后一包
int MppDecoder::Decode(uint8_t *pkt_data, int pkt_size, int pkt_eos)
{
    // 指向循环状态数据的指针，简化后续访问
    MpiDecLoopData *data = &loop_data;
    // 标记当前输入包是否已完全送入解码器（0=未完成，1=完成）
    RK_U32 pkt_done = 0;
    // MPP 操作是否成功返回值
    MPP_RET ret = MPP_OK;
    // 获取 MPP 上下文
    MppCtx ctx = data->ctx;
    // 获取 MPI 接口表
    MppApi *mpi = data->mpi;
    // 记录本次解码调用中成功解码出的帧数
    int got_frames = 0;
    // 如果成员变量 packet 尚未初始化
    if (packet == NULL)
    {
        // 初始化一个空的 MPP 数据包
        ret = mpp_packet_init(&packet, NULL, 0);
    }
    
    // 设置数据包的数据缓冲区指针
    mpp_packet_set_data(packet, pkt_data);
    // 设置数据包的总大小（字节数）
    mpp_packet_set_size(packet, pkt_size);
    // 设置数据包的当前读取位置指针（指向数据起始）
    mpp_packet_set_pos(packet, pkt_data);
    // 设置数据包的有效数据长度（通常与 size 相同）
    mpp_packet_set_length(packet, pkt_size);
    // 如果这是最后一包数据
    if (pkt_eos)
    // 设置数据包的 EOS（End Of Stream）标志
        mpp_packet_set_eos(packet);

    // 外层循环：确保输入包被完全送入解码器
    do
    {
        // 内层重试次数（用于超时处理）
        RK_S32 times = 5;
        // 如果当前包尚未完全送入解码器
        if (!pkt_done)
        {
            // 将数据包送入解码器输入队列
            ret = mpi->decode_put_packet(ctx, packet);
            // 如果成功，标记包已送入
            if (MPP_OK == ret)
                // 包如果成功送入标记 1
                pkt_done = 1;
        }
        
        // 内层循环：反复获取解码输出帧，直到没有更多帧输出
        do
        {
            // 标记本次循环是否获取到了一帧
            RK_S32 get_frm = 0;
            // 标记获取到的帧是否为最后一帧
            RK_U32 frm_eos = 0;
        // 标签，用于超时重试 
        try_again:
            // 从解码器输出队列获取一帧解码后的图像
            ret = mpi->decode_get_frame(ctx, &frame);
            // 如果返回超时错误
            if (MPP_ERR_TIMEOUT == ret)
            {
                // 如果还有重试次数
                if (times > 0)
                {
                    // 减少重试次数
                    times--;
                    // 休眠 2 毫秒（2000 微秒）
                    usleep(2000);
                    // 跳转到标签重新尝试获取帧
                    goto try_again;
                }
            }

            // 如果返回错误且不是超时（或超时次数用完）
            if (MPP_OK != ret)
            {
                // 跳出内层循环
                break;
            }

            // 成功获取到一帧
            if (frame)
            {
                // 获取该帧所需的缓冲区大小（用于内存分配）
                RK_U32 buf_size = mpp_frame_get_buf_size(frame);
                // 如果帧携带信息改变标志（如分辨率、格式变化）
                if (mpp_frame_get_info_change(frame))
                {
                    // 如果帧缓冲区组尚未创建
                    if (NULL == data->frm_grp)
                    {
                        // 获取内部 DRM 缓冲区组
                        ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret)
                        {
                            std::cerr << "get mpp buffer group failed ret " << ret << std::endl;
                            break;
                        }
                        // 设置解码器使用外部缓冲区组
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
                        if (ret)
                        {
                            std::cerr << "set buffer group failed ret " << ret << std::endl;
                            break;
                        }
                    }
                    else
                    {
                        // 如果缓冲区组已存在，则先清空
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret)
                        {
                            std::cerr << "clear buffer group failed ret " << ret << std::endl;
                            break;
                        }
                    }
                    
                    // 配置缓冲区组大小：每个缓冲区 buf_size，最多 24 个缓冲区
                    ret = mpp_buffer_group_limit_config(data->frm_grp, buf_size, 24);
                    if (ret)
                    {
                        std::cerr << "limit buffer group failed ret " << ret << std::endl;                      
                        break;
                    }
                    
                    // 通知解码器信息变化已处理完毕，可以继续解码
                    ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret)
                    {
                        std::cerr << "info change ready failed ret " << ret << std::endl;
                        break;
                    }
                }
                // 正常帧（无信息改变）
                else
                {
                    // 获取帧的水平 stride（一行像素占用的字节数，通常对齐到 16）
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    // 获取帧的垂直 stride（有效行数，可能对齐）
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                    // 获取帧的实际宽度（像素）
                    RK_U32 hor_width = mpp_frame_get_width(frame);
                     // 获取帧的实际高度（像素）
                    RK_U32 ver_height = mpp_frame_get_height(frame);
                    // 成功解码出一帧，计数器加 1
                    got_frames++;

                    // 如果上层设置了回调函数
                    if (callback != nullptr)
                    {
                        // 获取帧的像素格式（如 NV12、YUV420P 等）
                        MppFrameFormat format = mpp_frame_get_fmt(frame);
                         // 获取帧数据缓冲区的虚拟地址指针
                        char *data_vir = (char *)mpp_buffer_get_ptr(mpp_frame_get_buffer(frame));
                        // 调用回调函数，将解码后的帧信息传递给上层
                        // 参数依次为：用户数据、水平 stride、垂直 stride、宽度、高度、格式、保留字段0、数据指针、解码器ID
                        callback(this->userdata, hor_stride, ver_stride, hor_width, ver_height, format, 0, data_vir, this->id);
                    }
                }
                // 检查该帧是否为 EOS 帧
                frm_eos = mpp_frame_get_eos(frame);
                // 释放当前帧资源
                ret = mpp_frame_deinit(&frame);
                // 指针置空
                frame = NULL;
                // 标记已获取一帧
                get_frm = 1;
            }

            // 如果这是输入最后一包（pkt_eos）且包已送入解码器（pkt_done）但还未收到 EOS 帧
            if (pkt_eos && pkt_done && !frm_eos)
            {
                // 休眠 1 毫秒，等待解码器输出最后一帧
                usleep(1 * 1000);
                // 继续循环尝试获取帧
                continue;
            }

            // 如果收到 EOS 帧
            if (frm_eos)
            {
                // 跳出内层循环
                break;
            }

             // 如果本次循环获取到了帧，继续尝试获取下一帧
            if (get_frm)
                continue;
                
            break;
        // 内层循环结束
        } while (1);
        // 如果输入包已完全送入解码器，退出外层循环
        if (pkt_done)
            break;
        // 休眠 3 毫秒，等待解码器处理数据 
        usleep(3 * 1000);
    // 外层循环结束
    } while (1);

    // 释放 MPP 数据包资源（每次 Decode 调用后都会释放并重新初始化）
    mpp_packet_deinit(&packet);
    // 返回是否成功解码出至少一帧（true 表示有输出帧，false 表示无输出帧）
    return got_frames > 0;
}