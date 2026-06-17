#ifndef _RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_   // 头文件保护宏：如果未定义，则定义，防止重复包含
#define _RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_   // 定义该宏，表示头文件已被包含

#include <stdint.h>                            // 包含标准整数类型定义（如int8_t, int32_t等）
#include <vector>                              // 包含标准向量容器，用于动态数组

#define OBJ_NAME_MAX_SIZE 16                   // 定义对象名称的最大长度为16字节
#define OBJ_NUMB_MAX_SIZE 64                   // 定义每张图像最多检测的对象数量为64个
// #define OBJ_CLASS_NUM     1                 // 被注释：对象类别数量，原可能为1
#define NMS_THRESH        0.45                 // 定义NMS（非极大值抑制）的阈值，用于去重重叠框
#define BOX_THRESH        0.25                 // 定义检测框的置信度阈值，低于此值的框被过滤
// #define PROP_BOX_SIZE     (5+OBJ_CLASS_NUM) // 被注释：每个检测框的属性向量长度（5个坐标+类别数）

// 定义边界框结构体
typedef struct _BOX_RECT
{
    int left;      // 边界框左边界x坐标
    int right;     // 边界框右边界x坐标
    int top;       // 边界框上边界y坐标
    int bottom;    // 边界框下边界y坐标
} BOX_RECT;

// 定义单个检测结果结构体
typedef struct __detect_result_t
{
    char name[OBJ_NAME_MAX_SIZE];   // 对象名称字符串（如“person”）
    BOX_RECT box;                   // 边界框坐标
    float prop;                     // 置信度（概率）
} detect_result_t;

// 定义检测结果组结构体，用于存储一帧图像的所有检测结果
typedef struct _detect_result_group_t
{
    int id;                                         // 组ID（可能对应图像ID或批次索引）
    int count;                                      // 实际检测到的对象数量
    detect_result_t results[OBJ_NUMB_MAX_SIZE];     // 存储检测结果的数组，最多OBJ_NUMB_MAX_SIZE个
} detect_result_group_t;

// 后处理函数，将模型输出转换为检测结果
// input0, input1, input2: 三个输出张量（通常对应边框坐标、置信度、类别得分）
// model_in_h, model_in_w: 模型输入尺寸（高度、宽度）
// conf_threshold: 置信度阈值（小于此值的框被过滤）
// nms_threshold: NMS阈值
// scale_w, scale_h: 从模型输入尺寸到原始图像尺寸的缩放比例（用于将框坐标映射回原图）
// qnt_zps: 量化零点（zero points）列表，用于反量化
// qnt_scales: 量化缩放因子（scales）列表，用于反量化
// group: 输出结果组指针
// class_num: 类别总数（包括背景？具体取决于模型）
int post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w,
                 float conf_threshold, float nms_threshold, float scale_w, float scale_h,
                 std::vector<int32_t> &qnt_zps, std::vector<float> &qnt_scales,
                 detect_result_group_t *group,int class_num);

// 释放后处理相关资源（如预分配的缓冲区等）
// class_num: 类别总数，可能用于释放按类别分配的内存
void deinitPostProcess(int class_num);

#endif //_RKNN_ZERO_COPY_DEMO_POSTPROCESS_H_   // 结束头文件保护宏