/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "detection_fusion_manager.h"   // 包含检测融合管理器的头文件声明
#include <iostream>                     // 包含标准输入输出流，可能用于调试输出
#include <chrono>                       // 包含高精度计时库，用于统计融合耗时

// 构造函数：初始化类别名称映射和统计信息
DetectionFusionManager::DetectionFusionManager() {
    // 初始化类别名称映射（0-3 分别对应人员、安全帽、疲劳、通话/玩手机）
    class_names_[0] = "Person";
    class_names_[1] = "Helmet";
    class_names_[2] = "Tired";
    class_names_[3] = "Call/Play";
    
    // 初始化统计信息结构体（全部清零）
    stats_ = FusionStats{};
}

// 析构函数：无需特殊资源释放（所有成员变量都是值类型或由外部管理）
DetectionFusionManager::~DetectionFusionManager() {
}

// 设置融合配置（如 IoU 阈值、模型权重等）
void DetectionFusionManager::setConfig(const FusionConfig& config) {
    config_ = config;   // 将传入的配置保存到成员变量
}

// 核心融合函数：将四个模型（人员、头盔、疲劳、通话）的检测结果融合为一个统一的检测列表
std::vector<FusedDetection> DetectionFusionManager::fuseDetections(
    const detect_result_group_t& person_results,    // 人员检测结果（全身框）
    const detect_result_group_t& helmet_results,    // 头盔检测结果（头部/帽框）
    const detect_result_group_t& tired_results,     // 疲劳检测结果
    const detect_result_group_t& callplay_results) { // 通话/玩手机检测结果
    
    // 记录开始时间，用于统计融合耗时
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 存储最终融合后的检测结果
    std::vector<FusedDetection> fused_detections;
    // 存储四个模型的原始检测结果（转换为统一格式 FusedDetection）
    std::vector<std::vector<FusedDetection>> model_detections(4);
    
    // ==================== 1. 转换各模型的检测结果 ====================
    // 人员检测结果转换
    for (int i = 0; i < person_results.count; i++) {
        const auto& det = person_results.results[i];           // 当前检测结果
        FusedDetection fused;                                  // 创建融合检测对象
        fused.class_id = 0;                                    // 类别ID：0表示人员
        fused.class_name = "Person";                           // 类别名称
        // 将检测框转换为 OpenCV Rect2f 格式（左上角坐标+宽高）
        fused.bbox = cv::Rect2f(det.box.left, det.box.top, 
                               det.box.right - det.box.left, 
                               det.box.bottom - det.box.top);
        fused.confidence = det.prop;                           // 置信度
        fused.source_models = {0};                             // 来源模型：仅来自模型0
        fused.model_confidences = {det.prop};                  // 各来源模型的置信度
        fused.timestamp = std::chrono::system_clock::now();    // 当前时间戳
        model_detections[0].push_back(fused);                  // 存入模型0的检测列表
    }
    
    // 安全帽检测结果转换
    for (int i = 0; i < helmet_results.count; i++) {
        const auto& det = helmet_results.results[i];
        FusedDetection fused;
        fused.class_id = 1;                                    // 类别ID：1表示头盔
        fused.class_name = "Helmet";
        fused.bbox = cv::Rect2f(det.box.left, det.box.top, 
                               det.box.right - det.box.left, 
                               det.box.bottom - det.box.top);
        fused.confidence = det.prop;
        fused.source_models = {1};
        fused.model_confidences = {det.prop};
        fused.timestamp = std::chrono::system_clock::now();
        model_detections[1].push_back(fused);
    }
    
    // 疲劳检测结果转换
    for (int i = 0; i < tired_results.count; i++) {
        const auto& det = tired_results.results[i];
        FusedDetection fused;
        fused.class_id = 2;                                    // 类别ID：2表示疲劳
        fused.class_name = "Tired";
        fused.bbox = cv::Rect2f(det.box.left, det.box.top, 
                               det.box.right - det.box.left, 
                               det.box.bottom - det.box.top);
        fused.confidence = det.prop;
        fused.source_models = {2};
        fused.model_confidences = {det.prop};
        fused.timestamp = std::chrono::system_clock::now();
        model_detections[2].push_back(fused);
    }
    
    // 通话/玩手机检测结果转换
    for (int i = 0; i < callplay_results.count; i++) {
        const auto& det = callplay_results.results[i];
        FusedDetection fused;
        fused.class_id = 3;                                    // 类别ID：3表示通话/玩手机
        fused.class_name = "Call/Play";
        fused.bbox = cv::Rect2f(det.box.left, det.box.top, 
                               det.box.right - det.box.left, 
                               det.box.bottom - det.box.top);
        fused.confidence = det.prop;
        fused.source_models = {3};
        fused.model_confidences = {det.prop};
        fused.timestamp = std::chrono::system_clock::now();
        model_detections[3].push_back(fused);
    }
    
    // ==================== 2. 跨模型融合 ====================
    // 寻找不同模型之间的重叠检测框，将它们融合为单个检测结果
    // used[model][det_idx] 标记该检测是否已被融合（已使用）
    std::vector<std::vector<bool>> used(4);          // 4个模型，每个模型有一个bool数组
    for (int i = 0; i < 4; i++) {
        used[i].resize(model_detections[i].size(), false);   // 初始全部为false（未使用）
    }

    // 遍历所有模型对 (model1, model2)，model1 < model2，避免重复
    for (int model1 = 0; model1 < 4; model1++) {
        for (int model2 = model1 + 1; model2 < 4; model2++) {
            // 遍历 model1 的每个检测框
            for (size_t idx1 = 0; idx1 < model_detections[model1].size(); idx1++) {
                if (used[model1][idx1]) continue;      // 已融合过，跳过

                const auto& det1 = model_detections[model1][idx1];
                // 遍历 model2 的每个检测框
                for (size_t idx2 = 0; idx2 < model_detections[model2].size(); idx2++) {
                    if (used[model2][idx2]) continue;  // 已融合过，跳过

                    const auto& det2 = model_detections[model2][idx2];
                    // 判断是否应该融合
                    bool should_fuse = false;
                    // 特殊处理 Person(0) + Helmet(1) 嵌套场景：使用 IoM 判断
                    if (model1 == 0 && model2 == 1) {
                        float iom = calculateIoM(det1.bbox, det2.bbox);
                        should_fuse = (iom >= config_.iom_threshold);   // 用 IoM 阈值
                    } else {
                        // 其他模型对使用常规 IoU 判断
                        float iou = calculateIoU(det1.bbox, det2.bbox);
                        should_fuse = (iou > config_.iou_threshold);
                    }
                    
                    if (should_fuse) {
                        // 融合两个检测结果
                        FusedDetection fused;
                        fused.class_id = det1.class_id;          // 使用第一个检测的类别（通常更合适）
                        fused.class_name = det1.class_name;
                        
                        // 融合边界框（加权平均）
                        std::vector<cv::Rect2f> boxes = {det1.bbox, det2.bbox};
                        std::vector<float> weights = {
                            config_.model_weights[model1],   // 模型1的权重
                            config_.model_weights[model2]    // 模型2的权重
                        };
                        fused.bbox = weightedFusion(boxes, weights);
                        
                        // 融合置信度（加权平均）
                        std::vector<float> confidences = {det1.confidence, det2.confidence};
                        fused.confidence = confidenceFusion(confidences, weights);
                        
                        // 记录来源模型和各自的置信度
                        fused.source_models = {model1, model2};
                        fused.model_confidences = {det1.confidence, det2.confidence};
                        fused.timestamp = std::chrono::system_clock::now();
                        
                        fused_detections.push_back(fused);   // 添加到融合结果列表
                        used[model1][idx1] = true;           // 标记 model1 的该检测已融合
                        used[model2][idx2] = true;           // 标记 model2 的该检测已融合
                        break;  // 该检测已融合，退出内层循环（不再与其他检测融合）
                    }
                }
            }
        }
    }
    
    // ==================== 3. 添加未融合的检测结果 ====================
    // 将所有未被融合且置信度高于阈值的检测框直接添加到最终结果
    for (int i = 0; i < 4; i++) {
        for (size_t j = 0; j < model_detections[i].size(); j++) {
            if (!used[i][j] && model_detections[i][j].confidence > config_.confidence_threshold) {
                fused_detections.push_back(model_detections[i][j]);
            }
        }
    }
    
    // ==================== 4. 应用非极大值抑制 (NMS) ====================
    // 去除同一类别中高度重叠的冗余框，保留置信度最高的
    if (config_.enable_nms) {
        std::vector<int> keep_indices = applyNMS(fused_detections);   // 获取保留的索引
        std::vector<FusedDetection> nms_detections;
        for (int idx : keep_indices) {
            nms_detections.push_back(fused_detections[idx]);          // 按保留索引重建列表
        }
        fused_detections = nms_detections;
    }
    
    // ==================== 5. 更新统计信息 ====================
    auto end_time = std::chrono::high_resolution_clock::now();                     // 融合结束时间
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time); // 耗时（微秒）
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);  // 锁保护统计信息
        stats_.total_detections = person_results.count + helmet_results.count + 
                                 tired_results.count + callplay_results.count;   // 原始检测总数
        stats_.fused_detections = fused_detections.size();                      // 融合后检测数
        stats_.fusion_time_ms = duration.count() / 1000.0;                       // 融合耗时（毫秒）
    }
    
    return fused_detections;   // 返回最终的融合检测结果列表
}

// 在图像上绘制融合后的检测框
void DetectionFusionManager::drawFusedDetections(cv::Mat& frame, 
                                                const std::vector<FusedDetection>& detections) {
    for (const auto& detection : detections) {
        // 根据类别选择不同的颜色和线宽
        cv::Scalar color;
        int thickness = 2;
        
        switch (detection.class_id) {
            case 0: // Person
                color = cv::Scalar(0, 255, 0);   // 绿色
                thickness = 3;                    // 人员框更粗
                break;
            case 1: // Helmet
                color = cv::Scalar(0, 0, 255);    // 红色
                thickness = 2;
                break;
            case 2: // Tired
                color = cv::Scalar(0, 255, 255);  // 黄色
                thickness = 2;
                break;
            case 3: // Call/Play
                color = cv::Scalar(255, 0, 0);    // 蓝色
                thickness = 2;
                break;
            default:
                color = cv::Scalar(128, 128, 128); // 灰色
                thickness = 1;
                break;
        }
        
        // 绘制矩形框
        cv::Rect rect(detection.bbox.x, detection.bbox.y, 
                     detection.bbox.width, detection.bbox.height);
        cv::rectangle(frame, rect, color, thickness);
        
        // 在框的下方绘制置信度条（长度与置信度成正比）
        int bar_width = detection.bbox.width;                       // 条宽度等于框宽度
        int bar_height = 4;                                         // 条高度4像素
        cv::Rect confidence_bar(detection.bbox.x, detection.bbox.y + detection.bbox.height,
                               bar_width * detection.confidence, bar_height); // 根据置信度缩放
        cv::rectangle(frame, confidence_bar, color, -1);            // 填充矩形（-1表示填充）
    }
}

// 计算两个矩形框的 IoU（交并比）
float DetectionFusionManager::calculateIoU(const cv::Rect2f& box1, const cv::Rect2f& box2) {
    // 计算交集矩形的坐标
    float x1 = std::max(box1.x, box2.x);
    float y1 = std::max(box1.y, box2.y);
    float x2 = std::min(box1.x + box1.width, box2.x + box2.width);
    float y2 = std::min(box1.y + box1.height, box2.y + box2.height);
    
    // 无交集
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);          // 交集面积
    float area1 = box1.width * box1.height;              // 框1面积
    float area2 = box2.width * box2.height;              // 框2面积
    float union_area = area1 + area2 - intersection;     // 并集面积
    
    return intersection / union_area;                    // IoU = 交集 / 并集
}

// 计算两个矩形框的 IoM（交最小面积比），用于 Person + Helmet 嵌套场景
// 公式：IoM = 交集面积 / min(area1, area2)，小框大半在大框内时 IoM 高
float DetectionFusionManager::calculateIoM(const cv::Rect2f& box1, const cv::Rect2f& box2) {
    // 计算交集矩形坐标（同上）
    float x1 = std::max(box1.x, box2.x);
    float y1 = std::max(box1.y, box2.y);
    float x2 = std::min(box1.x + box1.width, box2.x + box2.width);
    float y2 = std::min(box1.y + box1.height, box2.y + box2.height);
    
    if (x2 <= x1 || y2 <= y1) {
        return 0.0f;
    }
    
    float intersection = (x2 - x1) * (y2 - y1);          // 交集面积
    float area1 = box1.width * box1.height;              // 框1面积
    float area2 = box2.width * box2.height;              // 框2面积
    float min_area = std::min(area1, area2);             // 较小框的面积
    
    return (min_area > 0) ? (intersection / min_area) : 0.0f;
}

// 加权融合边界框：根据权重对多个矩形框进行加权平均
cv::Rect2f DetectionFusionManager::weightedFusion(const std::vector<cv::Rect2f>& boxes,
                                                 const std::vector<float>& weights) {
    if (boxes.empty()) return cv::Rect2f();   // 无框时返回空矩形
    
    float total_weight = 0.0f;
    float weighted_x = 0.0f, weighted_y = 0.0f;
    float weighted_width = 0.0f, weighted_height = 0.0f;
    
    for (size_t i = 0; i < boxes.size(); i++) {
        float weight = weights[i];               // 当前权重
        total_weight += weight;                  // 累加总权重
        
        weighted_x += boxes[i].x * weight;       // 加权 x 坐标
        weighted_y += boxes[i].y * weight;       // 加权 y 坐标
        weighted_width += boxes[i].width * weight;   // 加权宽度
        weighted_height += boxes[i].height * weight; // 加权高度
    }
    
    // 归一化
    return cv::Rect2f(weighted_x / total_weight,
                     weighted_y / total_weight,
                     weighted_width / total_weight,
                     weighted_height / total_weight);
}

// 加权融合置信度：对多个置信度进行加权平均
float DetectionFusionManager::confidenceFusion(const std::vector<float>& confidences,
                                              const std::vector<float>& weights) {
    if (confidences.empty()) return 0.0f;
    
    float total_weight = 0.0f;
    float weighted_confidence = 0.0f;
    
    for (size_t i = 0; i < confidences.size(); i++) {
        float weight = weights[i];                     // 当前权重
        total_weight += weight;                        // 累加总权重
        weighted_confidence += confidences[i] * weight; // 加权置信度
    }
    
    return weighted_confidence / total_weight;         // 归一化返回
}

// 应用非极大值抑制（NMS）：去除重叠严重的冗余框
std::vector<int> DetectionFusionManager::applyNMS(const std::vector<FusedDetection>& detections) {
    if (detections.empty()) return {};   // 空列表直接返回
    
    // 1. 按置信度降序排列（置信度高的优先保留）
    std::vector<std::pair<float, int>> confidences_with_indices;
    for (size_t i = 0; i < detections.size(); i++) {
        confidences_with_indices.push_back({detections[i].confidence, i});
    }
    // 排序：置信度高的在前
    std::sort(confidences_with_indices.begin(), confidences_with_indices.end(),
              [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                  return a.first > b.first;
              });
    
    std::vector<int> keep_indices;                    // 保留的索引列表
    std::vector<bool> suppressed(detections.size(), false);  // 标记是否已被抑制
    
    // 2. 遍历排序后的列表，保留未被抑制的框，并抑制与其重叠度过高的其他框
    for (const auto& conf_idx : confidences_with_indices) {
        int idx = conf_idx.second;          // 当前检测框的原始索引
        if (suppressed[idx]) continue;      // 已抑制，跳过
        
        keep_indices.push_back(idx);        // 保留该框
        
        // 遍历所有框，抑制与当前框 IoU 超过阈值的框
        for (size_t i = 0; i < detections.size(); i++) {
            if (i == idx || suppressed[i]) continue;   // 跳过自身和已抑制的框
            
            float iou = calculateIoU(detections[idx].bbox, detections[i].bbox);
            if (iou > config_.iou_threshold) {        // 超过阈值则抑制
                suppressed[i] = true;
            }
        }
    }
    
    return keep_indices;
}

// 获取当前融合统计信息（线程安全）
DetectionFusionManager::FusionStats DetectionFusionManager::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);   // 加锁保护统计信息
    return stats_;                                     // 返回副本
}