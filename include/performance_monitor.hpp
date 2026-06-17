#ifndef __PERFORMANCE_MONITOR_HPP__        // 头文件保护宏：如果未定义__PERFORMANCE_MONITOR_HPP__，则定义它，防止重复包含
#define __PERFORMANCE_MONITOR_HPP__        // 定义宏，表示头文件已被包含

#include <chrono>                          // 包含chrono库，用于高精度时间测量
#include <map>                             // 包含map容器，用于存储名称到时间点/时间向量的映射
#include <string>                          // 包含string类，用于监控点名称
#include <iostream>                        // 包含iostream，用于输出统计信息
// 注意：此处使用了std::vector，但缺少#include <vector>，可能会引起编译错误，建议补充

class PerformanceMonitor {                 // 定义性能监控类，用于测量代码块的执行时间并统计
public:
    PerformanceMonitor() {}                // 构造函数，无特殊初始化
    ~PerformanceMonitor() {}               // 析构函数，无特殊清理

    // 开始计时
    void start(const std::string& name) {  // 接收监控点名称，开始计时
        auto now = std::chrono::steady_clock::now(); // 获取当前时间点（稳定时钟，适合测量间隔）
        start_times_[name] = now;          // 将当前时间点存入映射表，键为监控点名称
    }

    // 结束计时并返回耗时（毫秒）
    double end(const std::string& name) {  // 接收监控点名称，结束计时并返回耗时
        auto now = std::chrono::steady_clock::now(); // 获取当前时间点
        auto it = start_times_.find(name); // 查找该监控点的开始时间记录
        if (it != start_times_.end()) {    // 如果找到
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count(); // 计算时间差，转换为毫秒
            times_[name].push_back(duration); // 将耗时存入该监控点的耗时列表
            start_times_.erase(it);        // 删除开始时间记录，避免重复使用
            return duration;               // 返回耗时（毫秒）
        }
        return 0;                          // 未找到开始记录，返回0
    }

    // 打印所有监控点的平均耗时
    void printStats() {                    // 输出所有监控点的统计信息
        std::cout << "\n=== Performance Statistics ===" << std::endl; // 打印标题
        for (const auto& pair : times_) {  // 遍历每个监控点
            const std::string& name = pair.first;    // 监控点名称
            const std::vector<double>& values = pair.second; // 耗时列表
            if (values.empty()) continue;  // 如果无样本，跳过

            double sum = 0;                // 总和
            for (double val : values) sum += val; // 累加所有耗时
            double avg = sum / values.size();     // 计算平均值

            std::cout << name << ": " << avg << " ms (" << values.size() << " samples)" << std::endl; // 输出名称、平均耗时和样本数
        }
        std::cout << "==============================" << std::endl; // 打印结尾分隔线
    }

    // 重置所有监控数据
    void reset() {                         // 清除所有计时记录和统计信息
        start_times_.clear();              // 清空开始时间映射
        times_.clear();                    // 清空耗时列表映射
    }

private:
    std::map<std::string, std::chrono::steady_clock::time_point> start_times_; // 存储每个监控点的开始时间点
    std::map<std::string, std::vector<double>> times_;                         // 存储每个监控点的所有耗时记录（毫秒）
};

#endif /* __PERFORMANCE_MONITOR_HPP__ */   // 结束头文件保护宏