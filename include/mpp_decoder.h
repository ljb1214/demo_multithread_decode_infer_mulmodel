#ifndef _MPP_DECODER_H_                     // 头文件保护宏：如果未定义_MPP_DECODER_H_，则定义它，防止重复包含
#define _MPP_DECODER_H_                     // 定义该宏，表示头文件已被包含

#include <stdint.h>                         // 包含标准整数类型定义，如uint8_t、uint32_t等
#include <functional>                       // 包含std::function，用于回调函数封装
#include <mutex>                            // 包含互斥锁，用于线程安全
#include <cstring>                          // 包含C风格字符串操作函数（如memcpy等）
#include <rockchip/mpp_frame.h>             // Rockchip MPP帧结构头文件
#include <rockchip/rk_mpi.h>                // Rockchip MPP MPI接口头文件

// 定义MPP解码器帧回调函数类型
typedef std::function<void(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)> MppDecoderFrameCallback;
// 参数说明：
//   userdata: 用户自定义数据指针
//   width_stride: 图像宽度步长（对齐后的宽度）
//   height_stride: 图像高度步长（对齐后的高度）
//   width: 实际图像宽度
//   height: 实际图像高度
//   format: 图像格式（如MPP_FMT_YUV420P等）
//   fd: 内存文件描述符（用于DMA或共享内存）
//   data: 指向图像数据的指针
//   id: 解码器实例ID

// 定义MPI解码循环数据结构体，用于维护解码状态
typedef struct
{
    MppCtx ctx;                  // MPP上下文句柄
    MppApi *mpi;                 // MPP API接口指针
    RK_U32 eos;                  // 文件结束标志（End Of Stream）
    MppBufferGroup frm_grp;      // 帧缓冲区组
    MppBufferGroup pkt_grp;      // 包缓冲区组
    MppPacket packet;            // MPP数据包
    MppFrame frame;              // MPP帧
    size_t max_usage;            // 最大内存使用量（用于统计）
} MpiDecLoopData;

// MPP解码器类
class MppDecoder
{
public:
    MppCtx mpp_ctx = NULL;           // MPP上下文句柄
    MppApi *mpp_mpi = NULL;          // MPP API接口指针
    MppDecoder();                    // 构造函数
    ~MppDecoder();                   // 析构函数

    // 初始化解码器
    // video_type: 视频编码类型（如MPP_VIDEO_CodingAVC for H.264）
    // fps: 帧率，用于控制解码速度（-1表示不限速）
    // userdata: 用户数据指针，将传递给回调函数
    // id: 解码器实例ID，用于区分多个实例
    int Init(int video_type, int fps, void *userdata, int id);
    // 重置解码器状态
    int Reset();
    // 设置回调函数
    int SetCallback(MppDecoderFrameCallback callback);
    // 解码一帧数据
    // pkt_data: 编码数据指针
    // pkt_size: 数据长度
    // pkt_eos: 是否为最后一帧（1表示结束）
    int Decode(uint8_t *pkt_data, int pkt_size, int pkt_eos);

private:
    MppParam mpp_param1 = NULL;        // MPP参数（用于解码器配置）
    RK_U32 need_split = 1;             // 是否需要MPP内部拆包（1表示需要）
    RK_U32 width_mpp;                  // MPP处理后的图像宽度（对齐后）
    RK_U32 height_mpp;                 // MPP处理后的图像高度（对齐后）
    MppCodingType mpp_type;            // 编码类型（如MPP_VIDEO_CodingAVC）
    size_t packet_size = 2400 * 1300 * 3 / 2;  // 预分配的包缓冲区大小（约4.68MB）
    MpiDecLoopData loop_data;          // 解码循环数据
    MppPacket packet = NULL;           // 当前解码包
    MppFrame frame = NULL;             // 当前解码帧
    MppDecoderFrameCallback callback;  // 解码完成回调函数
    int fps = -1;                      // 目标帧率（-1表示不限速）
    unsigned long last_frame_time_ms = 0; // 上一帧输出时间戳（用于帧率控制）
    void *userdata = NULL;             // 用户数据指针
    int id = 0;                        // 解码器实例ID
    std::mutex mtx;                    // 互斥锁，保证多线程安全
};

#endif // _MPP_DECODER_H_              // 结束头文件保护宏