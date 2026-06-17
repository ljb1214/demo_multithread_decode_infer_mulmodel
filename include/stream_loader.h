/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

 // 防止头文件重复包含
#pragma once
// 包裹C语言头文件，防止被C++名称修饰
extern "C"
{
#include <libavformat/avformat.h>          // FFmpeg 格式处理相关
#include <libavcodec/avcodec.h>            // FFmpeg 编解码相关
#include <libavcodec/bsf.h>                // FFmpeg 比特流过滤器
#include <libavutil/imgutils.h>            // 图像工具函数
#include <libavutil/rational.h>            // 有理数操作（用于时间基）
#include <libavutil/time.h>                // 时间相关函数
#include <libswscale/swscale.h>            // 软件缩放和颜色转换
}
#include "mpp_decoder.h"                   // 自定义mpp硬件编码器封装
#include <opencv2/opencv.hpp>              // opencv 图像处理库
#include <thread>                          // 多线程支持
#include <atomic>                          // 原子操作，保证线程安全
#include <functional>                      // 通用函数包装器
#include <queue>                           // 队列容器
#include <vector>                          // 向量容器(东动态数组)
#include <mutex>                           // 互斥锁
#include <condition_variable>              // 条件变量
#include <string>                          // C++标准字符串
using std::queue;                          // 简化写法
using std::vector;                         // 简化写法
#include "m_buffer.hpp"                    // 自定义图像缓冲区管理类

// 自定义解码回调函数，用于接收解码后的图像
//   userdata       - 用户数据指针，此处为 Mbuffer*（被强制转换为 void*）
//   width_stride - 解码器输出图像的宽度步长（可能包含对齐填充）
//   height_stride- 解码器输出图像的高度步长
//   width        - 图像实际宽度（像素）
//   height       - 图像实际高度（像素）
//   format       - 解码器输出的像素格式（如 NV12）
//   fd           - 文件描述符（通常用于共享内存）
//   data         - 解码后的图像数据指针（首地址）
//   id           - 流ID（用于标识是哪个视频流）
using MppDecoderFrameCallback = std::function<void(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)>;

// StreamLoader 类，负责单个视频流的打开、读取、解码，并管理图像数据
class StreamLoader
{
public:
    MppDecoder decoder;                    // MPP硬件解码器对象

    int videoStreamIndex;                  // 视频流在AVFormatCotext 中的索引值
    AVDictionary *options = NULL;          // ffmpeg 选项字典,设置缓冲区大小、传输协议、超时时间等，尤其对于网络流（如 RTSP）至关重要
    AVFormatContext *fmtCtx = NULL;        // ffmpeg 上下文，代表打开的输入流
    AVCodecParameters *codecPar = NULL;    // 视频流的编码参数

    AVBSFContext *bsf_ctx = NULL;          // 比特流过滤器上下文，用于H264，AVCC转 Annex B 格式
    const AVBitStreamFilter *bsf;          // 比特流过滤器对象(后面的 h264_mp4toannexb)

    bool got_key_frame = false;            // 是否已经获取到关键帧I B P
    AVPacket *temp_pkt;                    // 临时存储读取到的数据包
    int current_pkt_id = 0;                // 当前包的编号，配合抓包软件用于调试，追踪数据流
    int stream_loader_id;                  // 当前对象唯一标识符(标记多路视频流)
    std::string stream_url;                // 流地址RTMP RTSP 本地是视频
    int width = 0;                         // 视频宽度
    int height = 0;                        // 视频高度
    int status = 0;                        // 状态码 0为正常 其他为异常
    bool isnotAnnexB = false;              // 标记是否需要将 H264 转为 Annex B 格式
    MppDecoderFrameCallback callback;      // 解码回调函数，用于获取解码后的图像帧，返回上层

    Mbuffer buffer;                        // 管理图像数据缓冲区，包含YUV BGR 的缓存图像
    std::atomic<bool> stopFlag;            // 原子标志位，通知线程结束

    double source_fps_ = 25.0;             // 源视频默认帧率 25fps
    bool is_local_file_ = false;           // 标记是否为本地文件

    void close();                          // 关闭视频流，释放 ffmpeg 资源
    bool read_frame();                     // 读取一帧图像数据，并解码
    StreamLoader(const std::string &url, int id); // StreamLoader 类构造函数
    ~StreamLoader();                       // StreamLoader 类析构函数
    int open();                            // 打开视频流，ffmpeg 初始化
    void operator()();                     // 线程主函数，循环读取视频帧
    void update_queue();                   // 更新队列
};

// StreamLoaderManager 类，单例模式，管理多个 StreamLoader 对象和线程
class StreamLoaderManager
{
public:
    // -----------------------------------------------
    // 本地测试用例
    // char *url105 = "rtsp://admin:jhx12345@192.168.1.105:554/Streaming/Channels/101";
    // char *url104 = "rtsp://admin:jhx12345@192.168.1.104:554/Streaming/Channels/101";
    // vector<char *> urls = {url105, url104, url105, url104, url105, url104};
    // 预定义的视频流地址列表
    vector<std::string> urls = {
        "assets/videos/1.mp4",
        "assets/videos/2.mp4",
        "assets/videos/3.mp4",
        "assets/videos/4.mp4",
        "assets/videos/2.mp4",
        "assets/videos/3.mp4"
    };
    // 需要加载的视频流数量
    int num_stream = 4;
    // -----------------------------------------------
    // 禁止拷贝构造和赋值操作，确保单例的唯一性
    StreamLoaderManager(const StreamLoaderManager &) = delete;
    StreamLoaderManager &operator=(const StreamLoaderManager &) = delete;

    // 获取单例实例的静态方法
    static StreamLoaderManager &getInstance()
    {
        static StreamLoaderManager instance; // C++11 保证了静态局部变量的线程安全性
        return instance;
    }

    // 加载指定 id 的视频流
    void load_stream(int id);
    // 卸载指定 id 的视频流
    void unload_stream(int id);

    // 存储所有 StreamLoader 对象的指针
    vector<StreamLoader *> stream_loaders;
    // 存储对应的工作线程对象集合
    vector<std::thread> threads;

private:
    // 私有构造函数，防止从外部创建对象
    StreamLoaderManager()
    {
        std::cout << "StreamLoaderManager created" << std::endl;
    }

    // 私有析构函数，防止外部删除对象
    ~StreamLoaderManager()
    {
        std::cout << "StreamLoaderManager destroyed" << std::endl;
    }
};
