/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once                                   // 确保头文件只被编译一次

#include <opencv2/opencv.hpp>                  // 包含 OpenCV 核心库，用于图像处理
#include <thread>                              // 包含线程库，用于创建推流线程
#include <mutex>                               // 包含互斥锁，用于保护共享数据
#include <queue>                               // 包含队列容器，用于存储待推送的视频帧数据
#include <atomic>                              // 包含原子类型，用于线程安全的标志位
#include <vector>                              // 包含向量容器
#include "postprocess.h"                       // 包含后处理头文件，定义检测结果结构体
#include <condition_variable>                  // 包含条件变量，用于线程同步
#include "mpp_encoder.h"                       // 包含 MPP 硬件编码器封装类

// 结构体：用于封装一路视频流的一帧数据及其检测结果
struct StreamingData {
    int stream_id;                                          // 流 ID（多路时区分）
    cv::Mat frame;                                          // 当前帧的图像数据（OpenCV Mat 格式）
    detect_result_group_t person_results;                   // 人员检测结果（由 postprocess.h 定义）
    detect_result_group_t helmet_results;                   // 头盔检测结果
    detect_result_group_t tired_results;                    // 疲劳检测结果
    detect_result_group_t callplay_results;                 // 打电话检测结果
    std::chrono::system_clock::time_point timestamp;        // 该帧的时间戳
};

// 结构体：推流配置参数
struct StreamingConfig {
    std::string rtmp_url;           // RTMP 推流地址（如 rtmp://xxx/live/stream）
    std::string rtsp_url;           // RTSP 推流地址（如 rtsp://xxx/live/stream）
    int width = 1280;               // 视频宽度（像素）
    int height = 720;               // 视频高度（像素）
    int fps = 25;                   // 帧率（fps）
    int bitrate = 2000000;          // 目标码率（bps，2Mbps 示例）
    bool enable_rtmp = true;        // 是否启用 RTMP 推流
    bool enable_gb28181 = false;    // 是否启用 GB/T 28181（预留）
    bool enable_rtsp = false;       // 是否启用 RTSP 推流
    bool draw_detections = true;    // 是否在图像上绘制检测框（调试/可视化用）
};

// 类：流媒体管理器，负责接收数据帧、编码并推送至流媒体服务器
class StreamingManager {
public:
    StreamingManager();                                     // 构造函数
    ~StreamingManager();                                    // 析构函数

    bool initialize(const StreamingConfig& config);         // 使用给定配置初始化推流器
    void addStreamingData(const StreamingData& data);       // 向内部队列添加一帧待推送数据
    void startStreaming();                                  // 启动推流线程
    void stopStreaming();                                   // 停止推流线程

    bool isStreaming() const { return streaming_active_.load(); }   // 返回当前是否正在推流（原子读取）

    // 推流统计信息结构体
    struct StreamingStats {
        int frames_sent = 0;                // 已发送帧数
        int frames_dropped = 0;             // 丢弃帧数（队列满时）
        double fps = 0.0;                   // 实际推流帧率
        std::chrono::system_clock::time_point last_frame_time;   // 最后一帧发送时间
    };
    StreamingStats getStats() const;        // 获取当前推流统计信息

private:
    void streamingWorker();                 // 推流工作线程函数（循环从队列取帧并发送）
    void drawDetections(cv::Mat& frame, const StreamingData& data);   // 在图像上绘制检测框（根据配置）
    std::string createDetectionJSON(const StreamingData& data);       // 生成检测结果的 JSON 字符串（用于元数据）

    bool initializeRTMP();                  // 初始化 RTMP 推流上下文
    bool initializeRTSP();                  // 初始化 RTSP 推流上下文
    bool initializeOutput(const char* format_name, const std::string& output_url,
                          void*& opaque_context, bool use_rtsp_options); // 通用初始化函数（RTMP/RTSP）
    bool ensureEncoderInitialized();        // 确保硬件编码器已正确初始化
    bool encodeFrame(const cv::Mat& frame, std::vector<uint8_t>& encoded_frame,
                     int& packet_size);     // 将一帧图像编码为 H.264/H.265 数据包
    bool writeEncodedFrame(void* opaque_context, const uint8_t* data,
                           int size, const char* output_name);   // 向指定输出（RTMP/RTSP）写入编码后数据
    void closeOutput(void*& opaque_context, const char* output_name); // 关闭输出上下文

private:
    StreamingConfig config_;                                // 推流配置
    std::atomic<bool> streaming_active_;                    // 推流是否活跃（原子标志）
    std::atomic<bool> should_stop_;                         // 请求停止标志

    std::queue<StreamingData> streaming_queue_;             // 待推送帧队列（FIFO）
    std::mutex queue_mutex_;                                // 保护队列的互斥锁
    std::condition_variable queue_cv_;                      // 条件变量，用于通知新数据到达

    std::thread streaming_thread_;                          // 推流线程对象

    void* rtmp_context_;                                    // RTMP 输出上下文指针（具体类型由实现决定）
    void* rtsp_context_;                                    // RTSP 输出上下文指针
    MppEncoder* mpp_encoder_;                               // MPP 硬件编码器对象指针
    int64_t frame_index_;                                   // 帧序号（用于 PTS 等）

    mutable std::mutex stats_mutex_;                        // 保护统计数据的互斥锁
    StreamingStats stats_;                                  // 推流统计信息

    std::chrono::system_clock::time_point last_fps_time_;   // 上次计算 FPS 的时间点
    int frame_count_;                                       // 当前统计周期内的帧计数
};