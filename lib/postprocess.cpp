// 版权声明：Rockchip Electronics Co., Ltd. 拥有版权，采用 Apache 2.0 许可证
// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// 包含后处理函数声明
#include "postprocess.h"
// 包含路径工具函数，用于加载标签文件
#include "path_utils.h"

#include <math.h>      // 数学函数（expf, logf, fmax, fmin）
#include <stdint.h>    // 整数类型（int8_t等）
#include <stdio.h>     // 标准I/O（fopen, fclose, fgetc）
#include <stdlib.h>    // 内存分配（malloc, free, realloc）
#include <string.h>    // 字符串操作（strncpy）
#include <sys/time.h>  // 时间相关（本文件未直接使用）

#include <set>         // 集合容器
#include <vector>      // 向量容器

// 静态全局变量：存储类别标签名称，最多支持10个类别（但实际可通过class_num动态设置，这里固定为10可能不足）
// static char *labels[OBJ_CLASS_NUM];
static char *labels[10];

// YOLO 模型三个输出层的锚点尺寸（每个输出层3个锚点，每个锚点宽、高）
const int anchor0[6] = {10, 13, 16, 30, 33, 23};   // 第1层（stride=8）的锚点
const int anchor1[6] = {30, 61, 62, 45, 59, 119};  // 第2层（stride=16）的锚点
const int anchor2[6] = {116, 90, 156, 198, 373, 326}; // 第3层（stride=32）的锚点

// 内联函数：限制数值val在[min, max]范围内，并返回整型（实际返回int）
inline static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

// 从文件流中读取一行，动态分配内存，并返回字符串
// 参数：fp - 文件指针；buffer - 输出缓冲区指针（会被重新分配）；len - 输出长度
// 返回值：读取的行字符串（调用者需free），失败返回NULL
char *readLine(FILE *fp, char *buffer, int *len)
{
    int ch;
    int i = 0;
    size_t buff_len = 0;

    // 初始分配0字节，后面根据需要realloc
    buffer = (char *)malloc(buff_len + 1);
    if (!buffer)
        return NULL; // 内存分配失败

    // 逐字符读取直到换行符或文件结束
    while ((ch = fgetc(fp)) != '\n' && ch != EOF)
    {
        buff_len++;
        // 重新分配内存，大小增加1字节（预留\0）
        void *tmp = realloc(buffer, buff_len + 1);
        if (tmp == NULL)
        {
            free(buffer);
            return NULL; // 内存不足，释放已分配内存并返回
        }
        buffer = (char *)tmp;

        buffer[i] = (char)ch;
        i++;
    }
    buffer[i] = '\0'; // 字符串结束符

    *len = buff_len; // 返回实际字符数（不含\0）

    // 如果是文件结束且没有读取到任何字符，或文件错误，则释放内存并返回NULL
    if (ch == EOF && (i == 0 || ferror(fp)))
    {
        free(buffer);
        return NULL;
    }
    return buffer;
}

// 从文件中读取所有行，每行作为一个字符串存入lines数组，最多max_line行
int readLines(const char *fileName, char *lines[], int max_line)
{
    FILE *file = fopen(fileName, "r"); // 以只读方式打开文件
    char *s;
    int i = 0;
    int n = 0;

    if (file == NULL)
    {
        printf("Open %s fail!\n", fileName);
        return -1; // 打开失败
    }

    // 循环调用readLine读取每一行，直到文件结束或达到最大行数
    while ((s = readLine(file, s, &n)) != NULL)
    {
        lines[i++] = s; // 将读取的行存入数组
        if (i >= max_line)
            break;
    }
    fclose(file); // 关闭文件
    return i;     // 返回实际读取的行数
}

// 加载类别标签名称：从指定文件读取，存入label数组，最多class_num个
int loadLabelName(const char *locationFilename, char *label[], int class_num)
{
    printf("loadLabelName %s\n", locationFilename);
    readLines(locationFilename, label, class_num); // 直接调用readLines填充label数组
    return 0;
}

// 计算两个矩形框的交并比（IoU）
static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                              float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0); // 交集的宽度（+1 可能为像素对齐）
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0); // 交集的高度
    float i = w * h;                                                      // 交集面积
    // 并集面积 = 第一个框面积 + 第二个框面积 - 交集面积
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u); // 避免除以0
}

// 非极大值抑制（NMS），对指定类别filterId的框进行抑制，IoU大于threshold的框被标记为-1
static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order,
               int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        if (order[i] == -1 || classIds[i] != filterId) // 跳过无效或非当前类别的框
        {
            continue;
        }
        int n = order[i]; // 当前框的索引（在outputLocations中的位置）
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId) // 同样跳过无效或类别不匹配
            {
                continue;
            }
            // 获取两个框的坐标（左上角+宽高格式）
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold) // IoU超过阈值，抑制后一个框
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

// 快速排序（降序），同时维护索引数组indices，用于对置信度排序时保留原始位置
static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right)
    {
        key_index = indices[left];
        key = input[left];
        while (low < high)
        {
            while (low < high && input[high] <= key) // 从右向左找大于key的元素
            {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) // 从左向右找小于key的元素
            {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices); // 递归排序左半部分
        quick_sort_indice_inverse(input, low + 1, right, indices); // 递归排序右半部分
    }
    return low;
}

// Sigmoid激活函数
static float sigmoid(float x) { return 1.0 / (1.0 + expf(-x)); }

// unsigmoid（逆sigmoid），此处未使用
static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }

// 限制浮点数val在[min, max]范围内，并返回整型（向下取整）
inline static int32_t __clip(float val, float min, float max)
{
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

// 将浮点数量化为int8_t（仿射量化），公式：q = round(f32/scale) + zero_point
static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
{
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)__clip(dst_val, -128, 127);
    return res;
}

// 将int8_t反量化为浮点数，公式：f32 = (q - zp) * scale
static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) { return ((float)qnt - (float)zp) * scale; }

/**
 * @brief 处理输入数据并返回有效框的数量
 *
 * 根据给定的输入数据、锚点、网格高度、网格宽度、图像高度、图像宽度、步长、阈值、零点偏移和比例因子，
 * 计算目标框的坐标、目标概率和类别，并存储到相应的向量中。返回有效框的数量。
 *
 * @param input 输入数据指针（int8_t类型，量化后的特征图）
 * @param anchor 锚点数组
 * @param grid_h 网格高度
 * @param grid_w 网格宽度
 * @param height 图像高度（模型输入高度）
 * @param width 图像宽度（模型输入宽度）
 * @param stride 步长
 * @param boxes 目标框坐标向量（输出）
 * @param objProbs 目标概率向量（输出）
 * @param classId 目标类别向量（输出）
 * @param threshold 置信度阈值
 * @param zp 零点偏移
 * @param scale 比例因子
 *
 * @return 有效框的数量
 */
static int process(int8_t *input, int *anchor, int grid_h, int grid_w, int height, int width, int stride,
                   std::vector<float> &boxes, std::vector<float> &objProbs, std::vector<int> &classId, float threshold,
                   int32_t zp, float scale, int class_num)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;                         // 网格总点数
    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale); // 将阈值量化为int8_t用于比较

    // 遍历每个锚点（每个输出层有3个锚点）
    for (int a = 0; a < 3; a++)
    {
        // 遍历每个网格单元
        for (int i = 0; i < grid_h; i++)
        {
            for (int j = 0; j < grid_w; j++)
            {
                // 获取目标置信度（第4个通道，每个锚点有(5+class_num)个通道）
                int8_t box_confidence = input[((5 + class_num) * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= thres_i8) // 置信度大于阈值
                {
                    // 计算当前网格单元在输入特征图中的起始偏移量
                    int offset = ((5 + class_num) * a) * grid_len + i * grid_w + j;
                    int8_t *in_ptr = input + offset;

                    // 解码边界框坐标（相对值）
                    float box_x = (deqnt_affine_to_f32(*in_ptr, zp, scale)) * 2.0 - 0.5;
                    float box_y = (deqnt_affine_to_f32(in_ptr[grid_len], zp, scale)) * 2.0 - 0.5;
                    float box_w = (deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale)) * 2.0;
                    float box_h = (deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale)) * 2.0;

                    // 转换到原图坐标（相对于网格偏移，乘以步长）
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];     // 宽高平方后乘以锚点宽度
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1]; // 高度同理

                    // 转换为左上角坐标
                    box_x -= (box_w / 2.0);
                    box_y -= (box_h / 2.0);

                    // 找出最大类别的概率和索引
                    int8_t maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < class_num; ++k)
                    {
                        int8_t prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs)
                        {
                            maxClassId = k;
                            maxClassProbs = prob;
                        }
                    }

                    // 如果最大类别概率也超过阈值，则保存该检测框
                    if (maxClassProbs > thres_i8)
                    {
                        // 目标置信度 = 目标存在概率 * 类别概率
                        objProbs.push_back((deqnt_affine_to_f32(maxClassProbs, zp, scale)) * (deqnt_affine_to_f32(box_confidence, zp, scale)));
                        classId.push_back(maxClassId);
                        validCount++;
                        boxes.push_back(box_x);
                        boxes.push_back(box_y);
                        boxes.push_back(box_w);
                        boxes.push_back(box_h);
                    }
                }
            }
        }
    }
    return validCount;
}

// 后处理主函数，对三个输出层（stride=8,16,32）的量化结果进行解码、NMS，并将最终结果存入group结构体
int post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w, float conf_threshold,
                 float nms_threshold, float scale_w, float scale_h, std::vector<int32_t> &qnt_zps,
                 std::vector<float> &qnt_scales, detect_result_group_t *group, int class_num)
{
    static int init = -1; // 静态变量，确保标签只加载一次
    if (init == -1)
    {
        int ret = 0;
        // 使用path_utils中的resolve_existing_path查找标签文件，支持环境变量覆盖
        const std::string label_path = resolve_existing_path(
            {"assets/models/coco_80_labels_list.txt", "model/coco_80_labels_list.txt", "coco_80_labels_list.txt"},
            "MYDEMO_LABEL_PATH");
        ret = loadLabelName(label_path.c_str(), labels, class_num);
        if (ret < 0)
        {
            return -1; // 标签加载失败
        }

        init = 0;
    }
    // memset(group, 0, sizeof(detect_result_group_t)); // 可选清零（已注释）

    std::vector<float> filterBoxes;   // 存储所有候选框的坐标（x,y,w,h）
    std::vector<float> objProbs;      // 存储每个候选框的置信度
    std::vector<int> classId;         // 存储每个候选框的类别ID

    // 处理 stride=8 的输出层
    int stride0 = 8;
    int grid_h0 = model_in_h / stride0;
    int grid_w0 = model_in_w / stride0;
    int validCount0 = 0;
    validCount0 = process(input0, (int *)anchor0, grid_h0, grid_w0, model_in_h, model_in_w, stride0, filterBoxes, objProbs,
                          classId, conf_threshold, qnt_zps[0], qnt_scales[0], class_num);

    // 处理 stride=16 的输出层
    int stride1 = 16;
    int grid_h1 = model_in_h / stride1;
    int grid_w1 = model_in_w / stride1;
    int validCount1 = 0;
    validCount1 = process(input1, (int *)anchor1, grid_h1, grid_w1, model_in_h, model_in_w, stride1, filterBoxes, objProbs,
                          classId, conf_threshold, qnt_zps[1], qnt_scales[1], class_num);

    // 处理 stride=32 的输出层
    int stride2 = 32;
    int grid_h2 = model_in_h / stride2;
    int grid_w2 = model_in_w / stride2;
    int validCount2 = 0;
    validCount2 = process(input2, (int *)anchor2, grid_h2, grid_w2, model_in_h, model_in_w, stride2, filterBoxes, objProbs,
                          classId, conf_threshold, qnt_zps[2], qnt_scales[2], class_num);

    int validCount = validCount0 + validCount1 + validCount2; // 总候选框数
    if (validCount <= 0)
    {
        // printf("no object detect\n");
        return 0; // 无目标
    }

    // 创建索引数组，用于排序
    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }

    // 按置信度降序排序，indexArray记录排序后的原位置
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    // 获取所有出现过的类别ID
    std::set<int> class_set(std::begin(classId), std::end(classId));

    // 对每个类别分别执行NMS
    for (auto c : class_set)
    {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    int last_count = 0;
    group->count = 0;
    /* 遍历所有有效的检测目标框 */
    for (int i = 0; i < validCount; ++i)
    {
        // 如果索引无效或结果数量已达最大值，跳过
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i]; // 获取原始索引
        // 获取框的坐标（左上角+宽高）
        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];

        int id = classId[n];     // 类别ID
        float obj_conf = objProbs[i]; // 置信度

        // 将坐标从模型输入尺寸缩放到原始图像尺寸，并存入结果结构体
        group->results[last_count].box.left = (int)(clamp(x1, 0, model_in_w) / scale_w);
        group->results[last_count].box.top = (int)(clamp(y1, 0, model_in_h) / scale_h);
        group->results[last_count].box.right = (int)(clamp(x2, 0, model_in_w) / scale_w);
        group->results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) / scale_h);

        // 复制类别名称（注意：这里始终复制 labels[0]，可能是一个 bug，应该复制 labels[id]）
        strncpy(group->results[last_count].name, labels[0], OBJ_NAME_MAX_SIZE);

        // 注意：未将置信度存入结果结构体，可能需要添加

        last_count++;
    }
    group->count = last_count;

    return 0;
}

// 后处理资源释放：释放之前动态分配的标签字符串内存
void deinitPostProcess(int class_num)
{
    for (int i = 0; i < class_num; i++)
    {
        if (labels[i] != nullptr)
        {
            free(labels[i]);
            labels[i] = nullptr;
        }
    }
}