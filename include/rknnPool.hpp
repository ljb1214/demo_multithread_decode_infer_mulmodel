/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * 本源码仅授权用于学习与研究目的。
 * 未经作者书面许可，严禁用于商业用途、再次分发、转售、创建衍生作品。
 */

#ifndef _rknnPool_H    // 防止头文件被重复包含（避免编译报错）
#define _rknnPool_H
#include <vector>      // C++ 标准库：动态数组，用于存储数据
#include <iostream>    // C++ 标准库：输入输出，打印日志
#include <sys/mman.h>  // Linux 内存映射 API
#include <fcntl.h>     // Linux 文件控制 API
#include <unistd.h>    // Linux 标准函数：close, read 等

#include "rga.h"       // RGA 硬件图像加速模块（瑞芯微专用）
#include "im2d.h"
#include "RgaUtils.h"


#include "rknn_api.h"                  // RKNN NPU 推理核心头文件 
#include "postprocess.h"               // 后处理头文件：解析模型输出 → 检测框
#include "opencv2/core/core.hpp"       // OpenCV 图像处理核心库

#include "opencv2/imgcodecs.hpp"       // OpenCV 图像读写
#include "opencv2/imgproc.hpp"         // OpenCV 图像处理（缩放、颜色转换、画框）
#include "ThreadPool.hpp"              // 线程池：多线程并行推理  

#include "memory_pool.hpp"             // 内存池：复用内存，避免频繁申请释放
#include "performance_monitor.hpp"     // 性能监控：统计各阶段耗时

#include "dma_buffer.h"   // dma-buf 分配函数
#include <unistd.h>       // for close
#include <sys/mman.h>     // for munmap (已在前面包含，可省略)

using cv::Mat;       // 简化代码：不用每次写 cv::Mat
using std::queue;    // 简化代码：不用每次写 std::queue
using std::vector;   // 简化代码：不用每次写 std::vector


// 函数声明：从文件中读取一段数据
static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz);
// 函数声明：读取 RKNN 模型文件到内存
static unsigned char *load_model(const char *filename, int *model_size);

// RKNN 轻量级推理封装类
class rknn_lite
{
private:
    // RKNN 模型句柄（NPU 核心对象）
    rknn_context rkModel;
    // 存储模型文件的二进制数据
    unsigned char *model_data;
    // RKNN SDK 版本信息
    rknn_sdk_version version;
    // 模型的【输入】和【输出】节点数量
    rknn_input_output_num io_num;
    // 模型输入节点属性（宽、高、格式、类型）
    rknn_tensor_attr *input_attrs;
    // 模型输出节点属性
    rknn_tensor_attr *output_attrs;
    // RKNN 输入结构体（只使用 1 个输入）
    rknn_input inputs[1];
    // 通用返回值：保存函数执行结果（0=成功，负数=失败）
    int ret;
    // 图像通道数：默认 3（RGB/BGR）
    int channel = 3;
    // 模型要求的输入宽度
    int width = 0;
    // 模型要求的输入高度
    int height = 0;
    // 模型检测的类别总数（如 80 类）
    int class_num;
    // 模型 ID：用于区分多个模型
    int id;

    // 内存池对象：管理输入图像内存
    MemoryPool *input_pool;
    // NPU 输入缓冲区指针
    uint8_t *input_buffer;
    // 文件描述符（未使用，预留）
    int fd;
    // 性能监控器：统计推理各阶段耗时
    PerformanceMonitor monitor;

        // dma-buf 相关成员
    int input_dma_fd;           // dma-buf 文件描述符
    uint8_t* input_dma_virt;    // dma-buf 映射的虚拟地址
    size_t input_dma_size;      // dma-buf 大小

public:
    // 公共变量：外部传入的、要进行推理的原始图像
    Mat ori_img;

    // 核心推理接口
    // 输入：ori_img
    // 输出：detect_result_group（所有检测框）
    int interf(detect_result_group_t &detect_result_group);

    // 构造函数：初始化模型、NPU、内存
    // 参数：模型路径、NPU核心号、类别数、模型ID
    rknn_lite(const char *dst, int n, int class_num, int id);

    // 析构函数：自动释放所有资源
    ~rknn_lite();

    // 打印性能统计
    void printStats() { monitor.printStats(); }
    // 重置性能统计
    void resetStats() { monitor.reset(); }
};

// ==============================================
// 构造函数：加载模型 + 初始化 NPU
// ==============================================
rknn_lite::rknn_lite(const char *model_name, int n, int class_num, int id)
{
    
    // 将类别数保存到成员变量
    this->class_num = class_num;
    // 将模型 ID 保存到成员变量
    this->id = id;

    // 打印日志：正在加载第几个模型
    printf("Loading model id = %d\n", id);

    // 模型文件大小
    int model_data_size = 0;
    // 调用函数：读取模型文件到内存
    model_data = load_model(model_name, &model_data_size);

    // 初始化 RKNN 引擎
    // 参数：模型句柄、模型数据、数据大小、标志、扩展参数
    ret = rknn_init(&rkModel, model_data, model_data_size, 0, NULL);
    // 判断是否初始化失败
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        // 失败直接退出程序
        exit(-1);
    }

    // 定义 NPU 核心掩码
    rknn_core_mask core_mask;
    // 根据传入的 n 选择 NPU 核心 0/1/2
    if (n == 0)
        core_mask = RKNN_NPU_CORE_0;
    else if (n == 1)
        core_mask = RKNN_NPU_CORE_1;
    else
        core_mask = RKNN_NPU_CORE_2;

    // 设置 RKNN 使用哪个 NPU 核心
    ret = rknn_set_core_mask(rkModel, core_mask);
    if (ret < 0)
    {
        printf("rknn_set_core_mask error ret=%d\n", ret);
        exit(-1);
    }

    // 查询 RKNN SDK 版本信息
    ret = rknn_query(rkModel, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        exit(-1);
    }

    // 查询模型：输入节点数量、输出节点数量
    ret = rknn_query(rkModel, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        exit(-1);
    }

    // 动态申请内存：存储所有输入节点属性
    input_attrs = new rknn_tensor_attr[io_num.n_input];
    // 将内存全部清零
    memset(input_attrs, 0, sizeof(input_attrs));

    // 遍历所有输入节点，获取属性
    for (int i = 0; i < io_num.n_input; i++)
    {
        // 设置当前节点索引
        input_attrs[i].index = i;
        // 查询节点信息
        ret = rknn_query(rkModel, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            exit(-1);
        }
    }

    // 动态申请内存：存储所有输出节点属性
    output_attrs = new rknn_tensor_attr[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));

    // 遍历所有输出节点，获取属性
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(rkModel, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
    }

    // 判断输入格式：NCHW 或 NHWC
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        // NCHW：通道在前
        channel = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        width = input_attrs[0].dims[3];
    }
    else
    {
        // NHWC：通道在后（OpenCV 默认格式）
        height = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }

    // 将输入结构体全部清零
    memset(inputs, 0, sizeof(inputs));
    // 设置输入节点索引：0 号输入
    inputs[0].index = 0;
    // 输入数据类型：8 位无符号整数
    inputs[0].type = RKNN_TENSOR_UINT8;
    // 输入数据总大小 = 宽 × 高 × 通道
    inputs[0].size = width * height * channel;
    // 输入格式：NHWC
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    // 不跳过预处理
    inputs[0].pass_through = 0;
    
    // 计算输入缓冲区大小
    input_dma_size = width * height * channel;
    // 尝试分配 dma-buf
    input_dma_fd = alloc_dma_buffer(input_dma_size, &input_dma_virt);
    if (input_dma_fd >= 0) {
        // 分配成功：使用 dma-buf 的虚拟地址作为输入缓冲区
        input_buffer = input_dma_virt;
        // 不再需要内存池，可以删除（为了降级保留，但此处不分配内存池）
        input_pool = nullptr;
    } else {
        // 分配失败，降级使用内存池
        fprintf(stderr, "Failed to allocate dma-buf for input buffer, fallback to memory pool\n");
        input_pool = new MemoryPool(width * height * channel, 4);
        input_buffer = (uint8_t*)input_pool->allocate();
        input_dma_fd = -1;
        input_dma_virt = nullptr;
        input_dma_size = 0;
    }
    // 文件描述符默认 -1（未使用）
    fd = -1;
}

// ==============================================
// 析构函数：程序结束时自动释放所有资源
// ==============================================
rknn_lite::~rknn_lite()
{
    // 销毁 RKNN 模型，释放 NPU 资源
    ret = rknn_destroy(rkModel);
    // 释放输入属性数组
    delete[] input_attrs;
    // 释放输出属性数组
    delete[] output_attrs;
    // 释放模型文件内存
    if (model_data)
        free(model_data);
    // 释放内存池中的内存
    if (input_buffer)
        input_pool->deallocate(input_buffer);
    // 销毁内存池对象
    delete input_pool;
        // 释放 dma-buf 资源
    if (input_dma_fd >= 0) {
        ::close(input_dma_fd);
        if (input_dma_virt) {
            munmap(input_dma_virt, input_dma_size);
        }
    }
}

// ==============================================
// 核心推理函数：输入图像 → 输出检测框
// ==============================================
int rknn_lite::interf(detect_result_group_t &detect_result_group)
{
    // 开始计时：整个推理流程总耗时
    monitor.start("total_infer");
    
    // 获取原始图像的宽度
    int img_width = ori_img.cols;
    // 获取原始图像的高度
    int img_height = ori_img.rows;
    
    // 开始计时：图像预处理阶段
    monitor.start("image_processing");
    
    rga_buffer_t src_buf = wrapbuffer_virtualaddr((void*)ori_img.data, img_width, img_height, RK_FORMAT_BGR_888);
    rga_buffer_t dst_buf;
    if (input_dma_fd >= 0) {
        // 使用 dma-buf fd 包装
        dst_buf = wrapbuffer_fd(input_dma_fd, width, height, width, height, RK_FORMAT_RGB_888);
    } else {
        // 降级使用虚拟地址
        dst_buf = wrapbuffer_virtualaddr(input_buffer, width, height, width, height, RK_FORMAT_RGB_888);
    }
    
    // RGA 硬件加速：缩放
    IM_STATUS status = imresize(src_buf, dst_buf);
    // 如果 RGA 执行失败
    if (status != IM_STATUS_SUCCESS) {
        // 降级方案：使用 OpenCV 处理
        cv::Mat img;
        // BGR 转 RGB
        cv::cvtColor(ori_img, img, cv::COLOR_BGR2RGB);
        // 如果尺寸不一致，进行缩放
        if (img_width != width || img_height != height)
            cv::resize(img, img, cv::Size(width, height));
        // 将处理后的数据复制到 NPU 输入缓冲区
        memcpy(input_buffer, img.data, width * height * channel);
    }
    
    // 结束计时：图像预处理完成
    monitor.end("image_processing");
    
    // 将处理好的图像数据绑定到 RKNN 输入
    inputs[0].buf = (void *)input_buffer;

    // 开始计时：设置输入数据
    monitor.start("set_input");
    // 将数据发送到 NPU
    rknn_inputs_set(rkModel, io_num.n_input, inputs);
    monitor.end("set_input");

    // 定义输出数组，数量 = 模型输出节点数
    rknn_output outputs[io_num.n_output];
    // 清零输出结构体
    memset(outputs, 0, sizeof(outputs));
    // 遍历所有输出
    for (int i = 0; i < io_num.n_output; i++)
        // 不使用浮点数，使用 int8 量化输出
        outputs[i].want_float = 0;
    
    // 开始计时：NPU 模型推理
    monitor.start("model_infer");
    // 执行 NPU 推理
    ret = rknn_run(rkModel, NULL);
    // 获取推理输出结果
    ret = rknn_outputs_get(rkModel, io_num.n_output, outputs, NULL);
    monitor.end("model_infer");

    // 后处理参数：NMS 非极大值抑制阈值
    const float nms_threshold = NMS_THRESH;
    // 后处理参数：置信度阈值
    const float box_conf_threshold = BOX_THRESH;

    // 计算缩放比例：模型输入宽度 / 原图宽度
    float scale_w = (float)width / img_width;
    // 计算缩放比例：模型输入高度 / 原图高度
    float scale_h = (float)height / img_height;

    // 定义两个数组，存储输出量化参数
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    // 遍历所有输出，获取量化参数
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    // 开始计时：后处理（解析检测框）
    monitor.start("post_process");
    // 调用后处理函数：将模型输出解析为检测框
    post_process(
        (int8_t *)outputs[0].buf,    // 输出 0
        (int8_t *)outputs[1].buf,    // 输出 1
        (int8_t *)outputs[2].buf,    // 输出 2
        height, width,               // 模型输入尺寸
        box_conf_threshold,           // 置信度阈值
        nms_threshold,                // NMS 阈值
        scale_w, scale_h,             // 缩放比例
        out_zps, out_scales,          // 量化参数
        &detect_result_group,         // 输出：检测结果
        class_num                     // 类别数量
    );
    monitor.end("post_process");

    // 开始计时：绘制检测框到原图
    monitor.start("draw_result");
    // 定义字符数组，存储显示文字
    char text[256];
    // 遍历所有检测到的目标
    for (int i = 0; i < detect_result_group.count; i++)
    {
        // 获取第 i 个检测结果
        detect_result_t *det_result = &(detect_result_group.results[i]);
        // 格式化字符串：类别名称 + 置信度百分比
        sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
        // 检测框左上角 X
        int x1 = det_result->box.left;
        // 检测框左上角 Y
        int y1 = det_result->box.top;
        
        // 根据模型 ID 选择框颜色
        if (id == 0)
            // 模型 0：绿色框，宽度 3
            rectangle(ori_img, cv::Point(x1, y1), cv::Point(det_result->box.right, det_result->box.bottom), cv::Scalar(0, 255, 0, 0), 3);
        else
            // 其他模型：红色框，宽度 3
            rectangle(ori_img, cv::Point(x1, y1), cv::Point(det_result->box.right, det_result->box.bottom), cv::Scalar(0, 0, 255, 0), 3);
    }
    monitor.end("draw_result");

    // 开始计时：释放输出资源
    monitor.start("release_output");
    // 释放 RKNN 输出内存
    ret = rknn_outputs_release(rkModel, io_num.n_output, outputs);
    monitor.end("release_output");
    
    // 结束计时：整个推理流程完成
    monitor.end("total_infer");

    // 返回 0：推理成功
    return 0;
}

// ==============================================
// 工具函数：从文件中读取指定偏移和大小的数据
// ==============================================
static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    // 定义数据指针
    unsigned char *data;
    // 函数返回值
    int ret;

    // 初始化为空
    data = NULL;

    // 如果文件指针为空，返回 NULL
    if (NULL == fp)
    {
        return NULL;
    }

    // 将文件指针移动到指定偏移量
    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    // 分配内存：大小为 sz
    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    // 从文件读取数据到内存
    ret = fread(data, 1, sz, fp);
    // 返回读取到的数据
    return data;
}

// ==============================================
// 工具函数：读取整个 RKNN 模型文件到内存
// ==============================================
static unsigned char *load_model(const char *filename, int *model_size)
{
    // 文件指针
    FILE *fp;
    // 存储模型数据
    unsigned char *data;

    // 以二进制只读方式打开文件
    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    // 将文件指针移到末尾
    fseek(fp, 0, SEEK_END);
    // 获取文件大小
    int size = ftell(fp);

    // 读取整个文件数据
    data = load_data(fp, 0, size);

    // 关闭文件
    fclose(fp);

    // 将文件大小输出
    *model_size = size;
    // 返回模型数据
    return data;
}

// 结束头文件保护
#endif