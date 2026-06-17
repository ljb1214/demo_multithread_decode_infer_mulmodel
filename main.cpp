

#include "dma_buffer.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include "stream_loader.h"
#include "rknnPool.hpp"
#include "streaming_manager.h"
#include "detection_fusion_manager.h"
#include "im2d.h"
#include "ThreadPool.hpp"
#include "performance_monitor.hpp"
#include "push.h"
#include "path_utils.h"
#ifdef ENABLE_GB28181
#include "gb28181.h"
#endif

namespace
{
bool env_enabled(const char *name, bool default_value)
{
    const std::string value = get_env_or_empty(name);
    if (value.empty())
        return default_value;
    return !(value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "OFF");
}
}

StreamLoaderManager &manager = StreamLoaderManager::getInstance();
// 创建RKNN模型的集合，用于存储多个模型实例
vector<rknn_lite *> rk_pool;
// 创建线程池对象，使用n个线程
vector<std::thread> rk_threads;
// 用于存储显示图像的Mat
vector<cv::Mat> images(6);
// 管理images的互斥锁
vector<std::mutex> mutexes(6);

// 全局线程池，用于推理任务
dpool::ThreadPool global_pool(8); // 8个线程，根据实际硬件调整

// 全局性能监控器
PerformanceMonitor global_monitor;

// 推流管理器
StreamingManager streaming_manager;

// 检测融合管理器
DetectionFusionManager fusion_manager;

static bool g_enable_gb28181 = false;

void combineImage(StreamLoaderManager &manager)
{
    const int tile_w = 640, tile_h = 360;
    const int tile_bgr_size = tile_w * tile_h * 3;
    int num_streams = manager.num_stream;  // 注意：如果 num_stream 可能变化，建议每次循环重新获取

    // 为每个流预分配 dma-buf 缓冲区（用于缩放后的 tile）
    std::vector<int> tile_fds(num_streams, -1);
    std::vector<uint8_t*> tile_virts(num_streams, nullptr);
    std::vector<size_t> tile_sizes(num_streams, 0);

    for (int i = 0; i < num_streams; ++i) {
        int fd = alloc_dma_buffer(tile_bgr_size, &tile_virts[i]);
        if (fd >= 0) {
            tile_fds[i] = fd;
            tile_sizes[i] = tile_bgr_size;
        } else {
            std::cerr << "Failed to allocate dma-buf for tile " << i << std::endl;
            // 降级：使用普通内存（后续会使用虚拟地址）
        }
    }

    cv::Mat combinedImage(1080, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat lastCombinedImage;
    bool hasLastFrame = false;
    const int target_fps = 24;
    const int frame_interval_ms = 1000 / target_fps;
    auto last_frame_time = std::chrono::steady_clock::now();

    while (true)
    {
        global_monitor.start("combine_image");

        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_frame_time).count();
        if (elapsed < frame_interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms - elapsed));
            current_time = std::chrono::steady_clock::now();
        }
        last_frame_time = current_time;

        bool hasNewFrame = false;

        // 注意：如果 num_streams 可能变化，应在此处重新获取
        // int num_streams = manager.num_stream;

        for (int i = 0; i < num_streams; ++i)
        {
            cv::Mat local_img;
            {
                std::lock_guard<std::mutex> lock(mutexes[i]);
                if (images[i].empty())
                    continue;
                local_img = std::move(images[i]);
                images[i] = cv::Mat();
            }

            // 检查源图像是否有效
            if (local_img.empty() || local_img.data == nullptr) {
                continue;   // 跳过无效帧
            }

            // 确保目标缓冲区有效：优先使用 dma-buf，否则创建普通 Mat
            cv::Mat resizedImage;
            bool use_dma_buf = (tile_fds[i] >= 0 && tile_virts[i] != nullptr);
            if (use_dma_buf) {
                // 使用预分配的 dma-buf 虚拟地址构造目标 Mat（不拷贝数据）
                resizedImage = cv::Mat(tile_h, tile_w, CV_8UC3, tile_virts[i]);
            } else {
                // 降级：使用普通内存
                resizedImage.create(tile_h, tile_w, CV_8UC3);
            }

            int src_w = local_img.cols, src_h = local_img.rows;

            // 再次确保源图像数据指针有效
            if (local_img.data == nullptr) {
                continue;
            }

            // 源缓冲区使用虚拟地址（因为 local_img 来自 OpenCV Mat）
            rga_buffer_t src_buf = wrapbuffer_virtualaddr(local_img.data, src_w, src_h, RK_FORMAT_BGR_888);
            // 目标缓冲区：优先使用 dma-buf fd，否则使用虚拟地址
            rga_buffer_t dst_buf;
            if (use_dma_buf) {
                dst_buf = wrapbuffer_fd(tile_fds[i], tile_w, tile_h, tile_w, tile_h, RK_FORMAT_BGR_888);
            } else {
                dst_buf = wrapbuffer_virtualaddr(resizedImage.data, tile_w, tile_h, tile_w, tile_h, RK_FORMAT_BGR_888);
            }

            // 执行 RGA 缩放，如果失败则降级到 OpenCV
            IM_STATUS status = imresize(src_buf, dst_buf);
            if (status != IM_STATUS_SUCCESS) {
                // 降级使用 OpenCV 软件缩放
                cv::resize(local_img, resizedImage, cv::Size(tile_w, tile_h));
            }

            // 将缩放后的图像复制到合成图的对应位置
            int row = i / 2, col = i % 2;
            int x = col * tile_w, y = row * tile_h;
            // 确保目标区域在 combinedImage 范围内
            if (x + tile_w <= combinedImage.cols && y + tile_h <= combinedImage.rows) {
                resizedImage.copyTo(combinedImage(cv::Rect(x, y, tile_w, tile_h)));
                hasNewFrame = true;
            } else {
                std::cerr << "Tile position out of bounds: i=" << i << ", x=" << x << ", y=" << y << std::endl;
            }
        }

        cv::Mat frameToSend;
        if (hasNewFrame) {
            lastCombinedImage = combinedImage.clone();
            frameToSend = lastCombinedImage;
            hasLastFrame = true;
        } else if (hasLastFrame) {
            frameToSend = lastCombinedImage;
        } else {
            frameToSend = combinedImage.clone();
        }

        StreamingData stream_data;
        stream_data.stream_id = 0;
        stream_data.frame = frameToSend;
        stream_data.timestamp = std::chrono::system_clock::now();
        memset(&stream_data.person_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.helmet_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.tired_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.callplay_results, 0, sizeof(detect_result_group_t));

        streaming_manager.addStreamingData(stream_data);

#ifdef ENABLE_GB28181
        if (g_enable_gb28181) {
            set_Mat(combinedImage);
        }
#endif

        global_monitor.end("combine_image");
    }

    // 释放 dma-buf 资源（理论上不会执行到这里）
    for (int i = 0; i < num_streams; ++i) {
        if (tile_fds[i] >= 0) {
            ::close(tile_fds[i]);
            if (tile_virts[i]) {
                munmap(tile_virts[i], tile_sizes[i]);
            }
        }
    }
    cv::destroyAllWindows();
}

void rknn_infer(rknn_lite *p1, rknn_lite *p2, rknn_lite *p3, rknn_lite *p4, int i)
{
    detect_result_group_t g1, g2, g3, g4;
    memset(&g1, 0, sizeof(detect_result_group_t));
    memset(&g2, 0, sizeof(detect_result_group_t));
    memset(&g3, 0, sizeof(detect_result_group_t));
    memset(&g4, 0, sizeof(detect_result_group_t));

    // 跳帧推理：每 INFER_INTERVAL 帧做一次完整推理，中间帧复用上一帧检测结果，提高实时性
    const int INFER_INTERVAL = 2; 
    int frame_count = 0;
    std::vector<FusedDetection> last_fused;

    while (!manager.stream_loaders[i]->stopFlag)
    {
        std::unique_lock<std::mutex> lock(manager.stream_loaders[i]->buffer.mtx);
        if (manager.stream_loaders[i]->buffer.img.empty())
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        p1->ori_img = manager.stream_loaders[i]->buffer.img.clone();
        p2->ori_img = p1->ori_img;
        if (p3) p3->ori_img = p1->ori_img;
        p4->ori_img = p1->ori_img;
        lock.unlock();

        frame_count++;
        bool do_infer = (frame_count % INFER_INTERVAL == 1) || last_fused.empty();

        if (do_infer)
        {
            auto f1 = global_pool.submit([&]() { p1->interf(g1); });
            auto f2 = global_pool.submit([&]() { p2->interf(g2); });
            std::future<void> f3;
            if (p3) f3 = global_pool.submit([&]() { p3->interf(g3); });
            auto f4 = global_pool.submit([&]() { p4->interf(g4); });

            f1.get(); f2.get();
            if (p3) f3.get();
            f4.get();

            last_fused = fusion_manager.fuseDetections(g1, g2, g3, g4);
        }

        fusion_manager.drawFusedDetections(p1->ori_img, last_fused);

        std::unique_lock<std::mutex> lockimage(mutexes[i]);
        images[i] = std::move(p1->ori_img);
        lockimage.unlock();
    }
}

int main(int argc, char *argv[])
{
    if (argc > 1)
        manager.num_stream = std::stoi(argv[1]);  //将char转化为int
    else
        manager.num_stream = static_cast<int>(manager.urls.size());

    if (argc > 2)
    {
        manager.urls.clear();
        for (int i = 2; i < argc; ++i)
            manager.urls.emplace_back(argv[i]);
        manager.num_stream = static_cast<int>(manager.urls.size());
    }

    if (manager.num_stream <= 0)
    {
        std::cerr << "num_stream must be greater than 0" << std::endl;
        return -1;
    }

    if ((int)manager.urls.size() < manager.num_stream)
    {
        std::cerr << "Not enough stream inputs. num_stream=" << manager.num_stream
                  << ", available urls=" << manager.urls.size() << std::endl;
        return -1;
    }

    const std::string model_person = resolve_existing_path(
        {"assets/models/person_relu.rknn", "model/person_relu.rknn", "person_relu.rknn"}, "MYDEMO_MODEL_PERSON");
    const std::string model_helmet = resolve_existing_path(
        {"assets/models/helmet_relu.rknn", "model/helmet_relu.rknn", "helmet_relu.rknn"}, "MYDEMO_MODEL_HELMET");
    const std::string model_callplay = resolve_existing_path(
        {"assets/models/callplay_relu.rknn", "model/callplay_relu.rknn", "callplay_relu.rknn"}, "MYDEMO_MODEL_CALLPLAY");
    
    // 初始化推流配置
    StreamingConfig stream_config;
    const std::string rtmp_url = get_env_or_empty("MYDEMO_RTMP_URL");
    const std::string rtsp_url = get_env_or_empty("MYDEMO_RTSP_URL");
    stream_config.rtmp_url = rtmp_url.empty() ? "rtmp://192.168.43.11/live/livestream" : rtmp_url;
    stream_config.rtsp_url = rtsp_url;
    stream_config.width = 1280;
    stream_config.height = 720;
    stream_config.fps = 24;
    stream_config.bitrate = 2000000;
    stream_config.enable_rtmp = env_enabled("MYDEMO_ENABLE_RTMP", true);
    stream_config.enable_rtsp = env_enabled("MYDEMO_ENABLE_RTSP", false);
    stream_config.enable_gb28181 = env_enabled("MYDEMO_ENABLE_GB28181", true);
    stream_config.draw_detections = true;

    g_enable_gb28181 = stream_config.enable_gb28181;

    // 启动 GB28181 推流（SIP/PS-RTP）
#ifdef ENABLE_GB28181
    if (g_enable_gb28181)
    {
        pushToGB28181();
    }
#else
    if (g_enable_gb28181)
    {
        std::cerr << "GB28181 support is not compiled in. Rebuild with ENABLE_GB28181 enabled." << std::endl;
    }
#endif
    
    // 初始化推流管理器
    if (!streaming_manager.initialize(stream_config)) {
        std::cerr << "Failed to initialize streaming manager" << std::endl;
        return -1;
    }
    
    // 启动推流
    streaming_manager.startStreaming();
    
    // 解码与推理线程
    for (int i = 0; i < manager.num_stream; ++i)
    {
        manager.load_stream(i);
        // 不同模型绑定不同 NPU 核心，使 ThreadPool 内 person/helmet/callplay 可并行推理
        rknn_lite *ptr1 = new rknn_lite(model_person.c_str(), 0, 1, 0);
        rknn_lite *ptr2 = new rknn_lite(model_helmet.c_str(), 1, 2, 1);
        rknn_lite *ptr3 = nullptr;
        rknn_lite *ptr4 = new rknn_lite(model_callplay.c_str(), 2, 2, 3);
        // 添加到模型池
        rk_pool.push_back(ptr1);
        rk_pool.push_back(ptr2);
        if (ptr3) rk_pool.push_back(ptr3);
        rk_pool.push_back(ptr4);
        rk_threads.push_back(std::thread(rknn_infer, ptr1, ptr2, ptr3, ptr4, i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    //启动画面拼接线程
    std::thread readerThread(combineImage, std::ref(manager));
    readerThread.join();

    // 停止 RTMP 推流
    streaming_manager.stopStreaming();

    // 停止 GB28181 推流
#ifdef ENABLE_GB28181
    if (g_enable_gb28181)
    {
        stopGB28181();
    }
#endif
    
    for (int i = 0; i < manager.num_stream; ++i)
    {
        manager.unload_stream(i);
    }
    for (auto &t : rk_threads)
    {
        if (t.joinable())
            t.join();
    }

    // 打印性能统计信息
    std::cout << "\n=== Global Performance Statistics ===" << std::endl;
    global_monitor.printStats();
    
    // 打印各个模型的性能统计信息
    for (auto model : rk_pool) {
        model->printStats();
    }

    return 0;
}
