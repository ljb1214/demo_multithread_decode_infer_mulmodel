/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */
#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <map>
#include <algorithm>
#include "postprocess.h"

// 融合后的检测结果
struct FusedDetection {
    int class_id;                    // 融合后的类别ID
    std::string class_name;          // 类别名称
    cv::Rect2f bbox;                // 融合后的边界框
    float confidence;                // 融合后的置信度
    std::vector<int> source_models; // 来源模型ID列表
    std::vector<float> model_confidences; // 各模型置信度
    std::chrono::system_clock::time_point timestamp;
};

// 融合配置
struct FusionConfig {
    float iou_threshold = 0.5;       // IoU阈值
    float iom_threshold = 0.3f;      // Person+Helmet 嵌套融合的 IoM 阈值
    float confidence_threshold = 0.3; // 置信度阈值
    bool enable_weighted_fusion = true; // 启用加权融合
    bool enable_nms = true;          // 启用NMS
    std::map<int, float> model_weights = { // 模型权重
        {0, 1.0},  // 人员检测
        {1, 0.9},  // 安全帽检测
        {2, 0.8},  // 疲劳检测
        {3, 0.7}   // 通话检测
    };
};

class DetectionFusionManager {
public:
    DetectionFusionManager();
    ~DetectionFusionManager();
    
    // 设置融合配置
    void setConfig(const FusionConfig& config);
    
    // 融合多个模型的检测结果
    std::vector<FusedDetection> fuseDetections(
        const detect_result_group_t& person_results,
        const detect_result_group_t& helmet_results,
        const detect_result_group_t& tired_results,
        const detect_result_group_t& callplay_results
    );
    
    // 绘制融合后的检测结果
    void drawFusedDetections(cv::Mat& frame, const std::vector<FusedDetection>& detections);
    
    // 获取融合统计信息
    struct FusionStats {
        int total_detections = 0;
        int fused_detections = 0;
        int suppressed_detections = 0;
        double fusion_time_ms = 0.0;
    };
    FusionStats getStats() const;

private:
    // 计算两个检测框的IoU
    float calculateIoU(const cv::Rect2f& box1, const cv::Rect2f& box2);
    // 嵌套场景 IoM = 交集/min(area1,area2)，用于 Person+Helmet
    float calculateIoM(const cv::Rect2f& box1, const cv::Rect2f& box2);
    
    // 加权融合检测框
    cv::Rect2f weightedFusion(const std::vector<cv::Rect2f>& boxes, 
                             const std::vector<float>& weights);
    
    // 置信度融合
    float confidenceFusion(const std::vector<float>& confidences,
                          const std::vector<float>& weights);
    
    // 非极大值抑制
    std::vector<int> applyNMS(const std::vector<FusedDetection>& detections);
    
    // 类别名称映射
    std::map<int, std::string> class_names_;

private:
    FusionConfig config_;
    mutable std::mutex stats_mutex_;
    FusionStats stats_;
};

