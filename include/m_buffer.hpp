/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

// 包含OpenCV核心库，提供图像处理功能（如cv::Mat）
#include <opencv2/opencv.hpp>
// 包含互斥锁头文件，用于线程同步
#include <mutex>
// 包含向量容器，用于动态数组管理
#include <vector>

struct Mbuffer {
    cv::Mat img;
    std::mutex mtx;
    int frame_interval_ms = 0;
    bool throttle = false;
    std::vector<uint8_t> yuv_work;
    cv::Mat bgr_work;
    int bgr_dma_fd = -1;
    uint8_t* bgr_dma_virt = nullptr;
    size_t bgr_dma_size = 0;
};