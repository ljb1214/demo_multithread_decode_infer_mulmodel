/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "dma_buffer.h"   // 如果已经创建了公共头文件
#include <unistd.h>       // for close
#include <sys/mman.h>     // for munmap
#include "stream_loader.h"      // 自定义视频流加载器头文件
#include "path_utils.h"         // 自定义路径工具库
#include "im2d.h"               // 瑞芯微RGA工具2D图像加速工具
#include <chrono>               // C++11引入时间库
#include <string>               // C++标准字符函数库
#include <thread>               // 多线程工具库
 
// 判断是否为 Annex B 格式
// 该函数并没有使用
int is_annexb(const uint8_t *buf, size_t buf_size) // buf是指向缓存区头地址的指针，buf_size缓存区的大小
{
    // 开头，至少需要四字节才能检查起始码
    if (buf_size >= 4)
    {
        // AnnexB格式以 0x000001 或 0x00000001 
        if ((buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) ||
            (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x01))
        {
            // 是 Annex B 格式
            return 1; 
        }
    }
    // 不是AnnexB格式
    return 0; 
}

// 硬件解码器回调函数，当一帧解码完成时被调用
// 参数说明：
//   buffer       - 用户数据指针，此处为 Mbuffer*（被强制转换为 void*）
//   width_stride - 解码器输出图像的宽度步长（可能包含对齐填充）
//   height_stride- 解码器输出图像的高度步长
//   width        - 图像实际宽度（像素）
//   height       - 图像实际高度（像素）
//   format       - 解码器输出的像素格式（如 NV12）
//   fd           - 文件描述符（通常用于共享内存）
//   data         - 解码后的图像数据指针（首地址）
//   id           - 流ID（用于标识是哪个视频流）
void mpp_decoder_frame_callback(void *buffer, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)
{
    // 1. 基本参数检查
    if (!buffer || !data) {
        fprintf(stderr, "Invalid callback parameters: buffer=%p, data=%p\n", buffer, data);
        return;
    }

    Mbuffer *mbuffer = (Mbuffer *)buffer;

    // 2. 检查 Mbuffer 中的关键成员是否有效（bgr_dma_fd 和 bgr_dma_virt 至少有一个可用）
    bool has_dma_buf = (mbuffer->bgr_dma_fd >= 0 && mbuffer->bgr_dma_virt != nullptr);
    bool has_bgr_work = (!mbuffer->bgr_work.empty() && mbuffer->bgr_work.data != nullptr);

    if (!has_dma_buf && !has_bgr_work) {
        fprintf(stderr, "No valid target buffer for RGA: fd=%d, virt=%p, bgr_work.empty=%d\n",
                mbuffer->bgr_dma_fd, mbuffer->bgr_dma_virt, mbuffer->bgr_work.empty());
        return;
    }

    // 3. 准备 YUV 数据缓冲区
    size_t yuv_size = (size_t)width * height * 3 / 2;
    if (mbuffer->yuv_work.size() < yuv_size) {
        mbuffer->yuv_work.resize(yuv_size);
    }
    uint8_t *yuv_data = mbuffer->yuv_work.data();
    if (!yuv_data) {
        fprintf(stderr, "yuv_data is null after resize\n");
        return;
    }

    // 4. 拷贝 YUV 数据（从 MPP 输出的 NV12 格式）
    uint8_t *base_y = (uint8_t *)data;
    uint8_t *base_c = base_y + (size_t)width_stride * height_stride;
    int idx = 0;
    for (int i = 0; i < height; i++, base_y += width_stride) {
        memcpy(yuv_data + idx, base_y, width);
        idx += width;
    }
    for (int i = 0; i < height / 2; i++, base_c += width_stride) {
        memcpy(yuv_data + idx, base_c, width);
        idx += width;
    }

    // 5. 强制使用 RGA3 核心（可选，如果 librga 支持）
    (void)imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1);

    // 6. 构造 RGA 源缓冲区：**强制使用虚拟地址**，避免 fd 无效导致崩溃
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(yuv_data, width, height, width, height, RK_FORMAT_YCbCr_420_SP);

    // 7. 构造 RGA 目标缓冲区
    rga_buffer_t dst_buf;
    if (has_dma_buf) {
        dst_buf = wrapbuffer_fd(mbuffer->bgr_dma_fd, width, height, width, height, RK_FORMAT_BGR_888);
    } else {
        dst_buf = wrapbuffer_virtualaddr(mbuffer->bgr_work.data, width, height, width, height, RK_FORMAT_BGR_888);
    }

    // 8. 执行 RGA 颜色转换
    IM_STATUS status = imcvtcolor(src_buf, dst_buf,
                                  RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                                  IM_COLOR_SPACE_DEFAULT);

    // 9. 如果 RGA 失败，降级使用 OpenCV
    if (status != IM_STATUS_SUCCESS) {
        static int fallback_count = 0;
        if (fallback_count++ < 3) {
            fprintf(stderr, "RGA NV12->BGR failed (%d), using OpenCV fallback\n", (int)status);
        }
        cv::Mat yuvMat(height + height / 2, width, CV_8UC1, yuv_data);
        cv::cvtColor(yuvMat, mbuffer->bgr_work, cv::COLOR_YUV2BGR_NV12);
    }

    // 10. 将结果图像移动到 mbuffer->img（线程安全）
    {
        std::unique_lock<std::mutex> mlock(mbuffer->mtx);
        // 如果 RGA 成功且使用了 dma-buf，则 bgr_work 可能未更新，需要从 dma-buf 构建 Mat
        if (status == IM_STATUS_SUCCESS && has_dma_buf) {
            cv::Mat bgr_mat(height, width, CV_8UC3, mbuffer->bgr_dma_virt);
            mbuffer->img = bgr_mat.clone();
        } else {
            mbuffer->img = std::move(mbuffer->bgr_work);
        }
    }

    // 11. 限速控制
    if (mbuffer->throttle && mbuffer->frame_interval_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(mbuffer->frame_interval_ms));
    }
}

// ... 上面的 mpp_decoder_frame_callback 定义保持不变 ...

// 关闭流，释放 ffmpeg 资源
void StreamLoader::close()
{
    decoder.Reset();

    if (temp_pkt) {
        av_packet_free(&temp_pkt);
    }

    if (fmtCtx) {
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
    }

    if (codecPar) {
        avcodec_parameters_free(&codecPar);
    }

    // 释放 dma-buf 资源
    if (buffer.bgr_dma_fd >= 0) {
        ::close(buffer.bgr_dma_fd);   // 加 :: 调用系统 close
        buffer.bgr_dma_fd = -1;
    }
    if (buffer.bgr_dma_virt) {
        munmap(buffer.bgr_dma_virt, buffer.bgr_dma_size);
        buffer.bgr_dma_virt = nullptr;
        buffer.bgr_dma_size = 0;
    }
}


//读取一帧数据包，解码并通过回调函数输出图像
bool StreamLoader::read_frame()
{
    //启用C++14的时间字面量如2ms
    using namespace std::chrono_literals;
    //连续读取失败计数，超过设置阈值后出发重连
    int eof_retry = 0;     
    // 已读视频包但解码未出帧的次数，防止异常时死循环        
    int no_frame_count = 0;  
    // EOF 时最多重试次数，超过则触发 reconnect    
    const int MAX_EOF_RETRY = 10;
    // 连续读包未出帧的上限，避免异常流导致死循环
    const int MAX_PACKETS_NO_FRAME = 100; 
    // 主循环：持续读取视频包直到成功解码一帧或遇到不可恢复错误
    while (true)
    {
        // 1.从输入流读取一个数据包存到temp_pkt
        int x = av_read_frame(fmtCtx, temp_pkt);
        // 读取失败
        if (x < 0)
        {
            // 存储状态码
            status = x;
            // 增加eof重连计数
            eof_retry++;
            // 如果重连次数超过设置最大阈值，表面流结束
            if (eof_retry >= MAX_EOF_RETRY) {
                return false;  // 确认 EOF，触发 reconnect 循环播放
            }
            // 短暂休眠
            std::this_thread::sleep_for(2ms);
            // 释放当前无效包
            av_packet_unref(temp_pkt);
            // 继续下一轮循环
            continue;
        }
        // 成功读到包，重置 EOF 计数
        eof_retry = 0;  
        // 2.检查当前数据包是否为视频包
        if (temp_pkt->stream_index != videoStreamIndex)
        {
            //释放音频类的非视频包
            av_packet_unref(temp_pkt);
            //继续读取
            continue;
        }
        // 3.如果视频包而且是H264格式需要比特流过滤器处理进行格式转换AVCC到Annex B
        if (isnotAnnexB)
        {
            // 将原始视频包送入过滤器
            int ret = av_bsf_send_packet(bsf_ctx, temp_pkt);
            // 如果失败
            if (ret < 0)
            {
                // 打印提示
                fprintf(stderr, "Error sending packet to filter\n");
                // 释放当前视频包
                av_packet_unref(temp_pkt);
                return false;
            }
            // 从过滤器接收转换后的包
            ret = av_bsf_receive_packet(bsf_ctx, temp_pkt);
            // 如果失败
            if (ret < 0)
            {
                // 打印提示
                fprintf(stderr, "Error receiving packet from filter\n");
                // 释放当前视频包
                av_packet_unref(temp_pkt);
                return false;
            }
        }
        // 4.将数据包送入硬件解码器进行解码
        bool decode_success = decoder.Decode(temp_pkt->data, temp_pkt->size, 0);
        // 不管解码释放成功，都释放当前包
        av_packet_unref(temp_pkt);
        // 如果解码成功则重置状态并返回true
        if (decode_success)
        {
            status = 0;
            return true;
        }
        // 解码未成功计数
        no_frame_count++;
        if (no_frame_count >= MAX_PACKETS_NO_FRAME)
        {
            // 异常：连续多包无输出，避免死循环
            return false;
        }
        //短暂休眠，避免死循环吃满CPU
        std::this_thread::sleep_for(2ms);
    }
}


// StreamLoadser 类的构造函数，用于初始化一个视频流加载器对象
// url 视频流源地址RTMP、RTSP或本地文件
// 分配给流加载器的唯一标识符
StreamLoader::StreamLoader(const std::string &url, int id)
{
    // 保存传入的流id到成员变量stream_load_id
    stream_loader_id = id;
    // 打印当前的流id，方便调试
    std::cout << "StreamLoader: " << std::to_string(id) << std::endl;
    // 设置解码回调函数为mpp_decoder_frame_callback
    // 当解码器输出一帧，会调用该函数，返回给上层使用
    callback = mpp_decoder_frame_callback;

                                            // mat_ptr = new cv::Mat();

    // 保存视频流地址url到成员变量stream_url
    stream_url = url;
    // 初始化状态码
    status = 0;
    // 初始化停止标志位，默认关闭
    stopFlag = false;
}

// 析构函数，销毁对象时自动调用，释放资源
StreamLoader::~StreamLoader()
{
    // 打印信息，方便调试
    std::cout << "destory stream loader: " << stream_loader_id << std::endl;
    // 调用close()函数，释放ffmpeg资源(AVPaket\AVFormatContext\解码器等资源)
    close();
                                           // delete mat_ptr;
                                           // 动态释放，对应前面注释行，未启用
}

//FFmpeg初始化、打开流并获取信息、查找视频流、初始化硬件解码器、H264特殊处理(格式转换)、设置回调、限速控制
int StreamLoader::open()
{
    // 1.初始化FFmpeg资源
    // 分配一个AVpaket结构体，用于存储读取到底视频包
    temp_pkt = av_packet_alloc(); 
    // 分配编码器参数结构，后面用于复制流参数
    codecPar = avcodec_parameters_alloc();

    // 2.设置RTSP拉流选项(AVFormatContext)的私有选项
    //设置接收缓冲区的小8MB
    av_dict_set(&options, "rtbufsize", "8192000", 0);
    //不启用实时流时间戳重置
    av_dict_set(&options, "start_time_realtime", 0, 0);
    //强制使用TCP传输，防止UDP丢包
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    //设置套字节超时时间2s
    av_dict_set(&options, "stimeout", "2000000", 0);
    //设置最大延迟0.5s
    av_dict_set(&options, "max_delay", "500000", 0);

    // 3.打开RTSP、RTMP、本地文件流俗称拉流
    //拉流api函数，参数分别是、上下文结构体指针、输入源地址、手动或自动(NULL)探测ffmpeg格式(flv）、设置协议层参数
    if (avformat_open_input(&fmtCtx, stream_url.c_str(), NULL, &options) != 0)
    {
        std::cout << "open rtsp stream failed" << std::endl;
        return -1;
    }
    // 4.查找RTSP、RTMP、本地文件流信息
    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
    {
        return -1;
    }

    // 5.打印视频相关调试信息
    av_dump_format(fmtCtx, 0, stream_url.c_str(), 0);
    
    // 6.获取视频流索引，并获取宽度、高度
    // 初始化为-1，表示未找到
    videoStreamIndex = -1;
    // 遍历所有流
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++)
    {
        // 检测器当前是否为视频流
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            // 记录视频流宽度
            width = fmtCtx->streams[i]->codecpar->width;
            // 记录视频流高度
            height = fmtCtx->streams[i]->codecpar->height;
            // 记录视频流索引
            videoStreamIndex = i;
            // 找到后退出循环
            break;
        }
    }
    // 输出找到后的索引
    std::cout << "videoindex: " << videoStreamIndex << std::endl;

    // 未找到视频流
    if (videoStreamIndex < 0)
    {
        // 未找到视频流索引，返回错误码
        return -2;
    }
    // 7.，根据视频流编码类型，初始化解码硬件
    AVCodecID rtsp_format = fmtCtx->streams[videoStreamIndex]->codecpar->codec_id;
    if (status == 0)
    {
        // 接收decoder.init的返回值
        int ret = 0;
        // 传递缓冲区给解码器，用于存放解码输出buffer是steramloader的成员，类型为Mbuffer
        void *src_buffer = &(this->buffer);
        switch (rtsp_format)
        {
        // 编码方式为H264
        case AV_CODEC_ID_H264:
            // 初始化H264解码器，参数分别为25帧、输出流、流ID
            ret = decoder.Init(264, 25, src_buffer, stream_loader_id);
            // ----------------------------------------------------------
            // 将AVCC格式转化为Annex B格式，因为硬件要求Annex B格式
            // 通过名称获取比特流过滤器对象
            bsf = av_bsf_get_by_name("h264_mp4toannexb");
            // 获取失败
            if (!bsf)
            {
                fprintf(stderr, "Could not find h264_mp4toannexb filter\n");
                // 关闭输入流，释放资源
                avformat_close_input(&fmtCtx);
                // 返回错误码
                return -3;
            }
            // AVBSF比特流过滤器，处理压缩后的比特流、用于AVCC和Annex B格式的相互转变
            // 分配比特流过滤器上下文，如果失败
            if (av_bsf_alloc(bsf, &bsf_ctx) < 0)
            {
                fprintf(stderr, "Could not allocate bsf context\n");
                //关闭输入流
                avformat_close_input(&fmtCtx);
                return -3;
            }
            // 将原始视频的编解码参数复制给过滤器的输入参数
            avcodec_parameters_copy(bsf_ctx->par_in, fmtCtx->streams[videoStreamIndex]->codecpar);
            // 设置输入时间基准
            bsf_ctx->time_base_in = fmtCtx->streams[videoStreamIndex]->time_base;

            // 初始化比特流过滤器
            if (av_bsf_init(bsf_ctx) < 0)
            {
                fprintf(stderr, "Could not initialize bsf context\n");
                // 释放资源
                av_bsf_free(&bsf_ctx);
                // 关闭输入流
                avformat_close_input(&fmtCtx);
                return -3;
            }
            
            // 标记视频流是否需要格式转换
            isnotAnnexB = true;
            // 输出初始化结果
            std::cout << "H264 " << ret << std::endl;
            break;
        case AV_CODEC_ID_HEVC:
            //硬件H265初始化，265就是HEVC，硬件直接支持Annex B格式，无需转换
            ret = decoder.Init(265, 25, src_buffer, stream_loader_id);
            // 打印初始化结果
            std::cout << "HEVC " << ret << std::endl;
            break;
        }
    }
    // 8.设置解码回调函数，当解码出一帧图像时，调用这个函数，返回上给层使用
    // callback是streamloader成员函数的指针
    decoder.SetCallback(this->callback);

    // 9.复制编辑码参数到codecpar，用于后续查询和处理，AVCodecParameters结构的成员变量
    avcodec_parameters_copy(codecPar, fmtCtx->streams[videoStreamIndex]->codecpar);

    // 10.获取源视频帧率，用于本地文件限速
    // 指向视频流的指针
    AVStream *st = fmtCtx->streams[videoStreamIndex]; 

    // 优先使用平均帧率（av_q2d 将 AVRational 转换为 double）
    double fps = av_q2d(st->avg_frame_rate);

    //如果平均帧率失效则使用实时帧率
    if (fps <= 0) fps = av_q2d(st->r_frame_rate);

    //  如果帧率失效则默认25fps
    if (fps <= 0) fps = 25.0;

    // 保持帧率到成员变量
    source_fps_ = fps;

    // 11.判断视频流是本地文件还是网络流
    is_local_file_ = false;

    // 检查 URL 是否以 rtsp:// 或 rtmp:// 开头
    if (stream_url.rfind("rtsp://", 0) != 0 && stream_url.rfind("rtmp://", 0) != 0)
        is_local_file_ = true;

    // 12.如果视频流文件是本地文件，则启用阿皮、函数throttle限制速度
    if (is_local_file_ && source_fps_ > 0) {
        // 开启限速标志
        buffer.throttle = true;
        // cframe_interval_ms 设为 (1000/fps)*1.2，略微放宽，避免卡顿
        buffer.frame_interval_ms = (int)(1000.0 / source_fps_ * 1.2);
    }

    // ... 之前的代码（设置限速等）...

// 为 bgr_work 预分配 dma-buf（用于 RGA 转换）
    size_t bgr_size = (size_t)width * height * 3;
    buffer.bgr_dma_fd = alloc_dma_buffer(bgr_size, &buffer.bgr_dma_virt);
    if (buffer.bgr_dma_fd < 0) {
        std::cerr << "Failed to allocate dma-buf for bgr_work, using regular Mat" << std::endl;
        buffer.bgr_dma_fd = -1;
        buffer.bgr_dma_virt = nullptr;
        buffer.bgr_dma_size = 0;
        // 创建普通的 cv::Mat
        buffer.bgr_work.create(height, width, CV_8UC3);
    } else {
        buffer.bgr_dma_size = bgr_size;
        buffer.bgr_work = cv::Mat(height, width, CV_8UC3, buffer.bgr_dma_virt);
    }


    return 0;
}

// 流加载器的线程主函数，重载了函数调用运算符，使得对象可以作为线程函数
void StreamLoader::operator()()
{
    //循环运行，知道stopflag被设置为true停止
    while (stopFlag == false)
    {
        //尝试读取一帧图像并解码
        try
        {
            // 核心函数，通过读取一个包，解码并通过回调输出图像
            read_frame();
        }
        // 捕获所有异常
        catch (std::exception &e)
        {
            // 输出打印信息并继续执行循环，线程不退出
            std::cout << "exception ............" << std::endl;
            std::cout << e.what() << std::endl;
        }
        // 标记检查状态，status非0表示发生错误或读取结束
        if (status)
        {
            // status < 0 通常为 AVERROR_EOF（文件播完），触发 reconnect 实现循环播
            std::cout << "Stream " << stream_loader_id << " EOF, reconnecting..." << std::endl;

            // 先关闭当前流，是否ffmpeg资源，清理解码器和 AVFormatContext
            close();
            // 根据 URL 类型区分本地文件与网络流：
            // - 本地文件：立即重新 open，相当于从头开始播放，实现循环播放
            // - 网络流（rtsp/rtmp 等）：按原来的逻辑，失败时 10 秒后重试

            bool is_network_stream = false;
            // 检查 URL 是否以 rtsp:// 或 rtmp:// 开头
            if (stream_url.rfind("rtsp://", 0) == 0 ||
                stream_url.rfind("rtmp://", 0) == 0)
            {
                is_network_stream = true;
            }

            //根据不同的流类型，执行不同的重连策略
            if (is_network_stream)
            {
                // 原有 RTSP 重连逻辑：失败则 10s 后重试，open返回0表示重连成功
                while (open() != 0)
                {
                    //打印重连信息
                    std::cout << "Reconnect (network) failed, retry after 10s, id = "
                              << stream_loader_id << std::endl;
                    //休眠10s避免反复重连浪费资源，避免cpu空转，降低功耗和发热，而且重新连接需要时间，立即重连可能会导致失败
                    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
                }
            }
            else
            {
                // 本地文件：立即重新 open，相当于从头开始播放
                // 如果打开失败，短暂等待后快速重试
                while (open() != 0)
                {
                    //打印重连信息
                    std::cout << "Reopen local file failed, retry shortly, id = "
                              << stream_loader_id << std::endl;
                    //短暂休眠100ms，即使只有 100ms 的休眠，也会让当前线程让出 CPU，使系统能调度其他线程或进程，提高整体系统的响应性。
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                //成功打开本地文件提升循环播放开始
                std::cout << "Local file reopened, loop playback, id = "
                          << stream_loader_id << std::endl;
            }

            // 重置状态，继续正常读取帧，重置状态为 0，表示流已成功重新打开，可以继续读取
            status = 0;
        }
    }
}

// ==================================================================================================

void StreamLoaderManager::load_stream(int id) //id的含义是数据流id
{
    // 输出加载开始的信息，包含数据流id
    std::cout << "Loading stream id: " << id << std::endl;
    // 检查数据流id的范围是否处于有效期间
    if (id < 0 || id >= (int)urls.size())
    {
        // 无效id，输出错误打印
        std::cerr << "Invalid stream id " << id << ", urls size = " << urls.size() << std::endl;
        return;
    }

    // 获取原始urls，rtmp或rtsp地址或本地路径
    std::string resolved_url = urls[id];
    // 如果url不是以rtsp://或rtmp://开头，则视为本地文件路径，需要解析实际存在的文件路径(.rfind是查找是否包含某字符串的是否存在的字符串函数)
    if (resolved_url.rfind("rtsp://", 0) != 0 && resolved_url.rfind("rtmp://", 0) != 0)
        // resolve_existing_path会查找候选路径（例如通过环境变量或预定义目录）
        resolved_url = resolve_existing_path({resolved_url});
    // 打印解析后的实际路径
    std::cout << "Resolved stream path/url: " << resolved_url << std::endl;
    // 创建StreamLoader的对象，用于具体流的打开，解码，回调
    StreamLoader *loader = new StreamLoader(resolved_url, id);
    // 调用open()函数，初始化FFmpeg格式上下文，解码器
    loader->open();
    // loader指针载入stream_loaders,方便后续操作(如加载时打开open(),卸载释放)
    stream_loaders.push_back(loader);
    // 创建线程执行loader的operater()(其中包含读取帧，解码，回调等流程)，并且使用std::ref的作用是确保传递的是Loader的引用避免拷贝，提高性能
    // threads.emplace_back(...) 直接在 vector 末尾构造一个 std::thread 对象,是一个典型的在多线程编程中传递不可拷贝对象的写法，确保线程安全地共享同一个对象
    threads.emplace_back(std::thread(std::ref(*loader)));
}

// 卸载指定ID的视频流
void StreamLoaderManager::unload_stream(int id)
{
    // 打印正在卸载的视频流id，方便调试
    std::cout << "Unloading stream id: " << id << std::endl;
    // 设置对应流加载器的停止标志为true，通知当前线程退出循环
    stream_loaders[id]->stopFlag = true;
    // 短暂休眠10s，让线程有足够时间检测到stopflag并开始退出
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // 如果该线程可以join，等待完全结束
    // 这是线程安全退出的标准写法
    if(threads[id].joinable())
        // 阻塞当前线程，直到目标线程执行完毕
        threads[id].join();
    // 释放当前对象流加载器对象
    delete stream_loaders[id];
}

/* ============================================================================
   函数调用顺序 / 模块协作关系
   ============================================================================

   StreamLoaderManager::load_stream(id)
   ├── new StreamLoader(url, id)
   │   └── StreamLoader::StreamLoader()              // 初始化成员
   ├── loader->open()
   │   ├── av_packet_alloc()                         // FFmpeg 分配包
   │   ├── avcodec_parameters_alloc()                // 分配编解码参数
   │   ├── av_dict_set() 多次                        // 设置 RTSP 选项
   │   ├── avformat_open_input()                     // 打开流
   │   ├── avformat_find_stream_info()               // 查找流信息
   │   ├── av_dump_format()                          // 打印流信息（调试）
   │   ├── 获取视频流索引及宽高
   │   ├── 根据编码类型：
   │   │   ├── AV_CODEC_ID_H264
   │   │   │   ├── decoder.Init(264, ...)            // 初始化解码器
   │   │   │   ├── av_bsf_get_by_name("h264_mp4toannexb") // 获取比特流过滤器
   │   │   │   ├── av_bsf_alloc()                   // 分配过滤器上下文
   │   │   │   ├── avcodec_parameters_copy()
   │   │   │   ├── av_bsf_init()                    // 初始化过滤器
   │   │   │   └── isnotAnnexB = true
   │   │   └── AV_CODEC_ID_HEVC
   │   │       └── decoder.Init(265, ...)           // 初始化解码器
   │   ├── decoder.SetCallback(mpp_decoder_frame_callback) // 设置回调
   │   ├── avcodec_parameters_copy()
   │   ├── 计算源帧率（用于本地文件限速）
   │   ├── 判断是否为本地文件并设置限速标志
   │   └── return
   ├── stream_loaders.push_back(loader)
   └── threads.emplace_back(std::thread(std::ref(*loader)))
       └── 启动线程执行 StreamLoader::operator()()

   StreamLoader::operator()()
   ├── while (stopFlag == false)
   │   ├── read_frame()
   │   │   ├── av_read_frame()                       // 读取一帧（包）
   │   │   ├── 检查流索引，跳过非视频包
   │   │   ├── 如果需要 AnnexB 转换（H264）：
   │   │   │   ├── av_bsf_send_packet()
   │   │   │   └── av_bsf_receive_packet()
   │   │   ├── decoder.Decode(pkt_data, pkt_size, 0) // 解码
   │   │   │   └── 解码成功后，回调 mpp_decoder_frame_callback()
   │   │   ├── av_packet_unref()                    // 释放包引用
   │   │   └── return true/false
   │   └── 若 read_frame 返回 false（EOF 或错误）：
   │       ├── close()                               // 释放当前资源
   │       │   ├── decoder.Reset()
   │       │   ├── av_packet_free(&temp_pkt)
   │       │   ├── avformat_close_input(&fmtCtx)
   │       │   └── avcodec_parameters_free(&codecPar)
   │       ├── 根据流类型重试 open()（循环直至成功）
   │       │   ├── 网络流：失败 sleep 10s 后重试
   │       │   └── 本地文件：快速重试，实现循环播放
   │       └── status = 0，继续循环

   mpp_decoder_frame_callback(buffer, width_stride, height_stride, width, height, format, fd, data, id)
   ├── 将解码数据拷贝到 mbuffer->yuv_work (YUV NV12)
   ├── mbuffer->bgr_work.create(height, width, CV_8UC3)  // 创建 BGR 图像
   ├── 尝试 RGA 硬件转换：
   │   ├── wrapbuffer_virtualaddr_t()                  // 包装源/目标缓冲区
   │   └── imcvtcolor()                                // 执行 NV12 → BGR 转换
   │       └── 若失败，使用 OpenCV fallback：
   │           └── cv::cvtColor(yuvMat, bgr_work, COLOR_YUV2BGR_NV12)
   ├── 加锁，将 bgr_work 移动到 mbuffer->img
   └── 如果限速标志启用，sleep(frame_interval_ms)

   StreamLoaderManager::unload_stream(id)
   ├── stream_loaders[id]->stopFlag = true
   ├── threads[id].join()
   └── delete stream_loaders[id]
       └── ~StreamLoader()
           └── close()  // 同上

   ============================================================================ */