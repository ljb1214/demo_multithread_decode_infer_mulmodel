#ifndef _DECODE_H_                         // 开始头文件宏
#define _DECODE_H_                         
#include <string.h>                        // 包含C标志库函数mmcpy、mmset
#include <sys/time.h>                      // 包含系统时间库
#include <stdlib.h>                        // C标准库
#include "frame_queue.h"                   // 自定义帧队列有关头文件
#include <rockchip/rk_mpi.h>               // 瑞芯微多媒体处理接口
#include <rockchip/mpp_frame.h>            // 瑞芯微帧结构定义头文件MPPFrame
#include <opencv2/opencv.hpp>              // opencv 库核心头文件
#include <atomic>                          // C++11 原子操作核心头文件库
#include <chrono>                          // C++11 时间函数库，高精度
#include <vector>                          // C++ 标准数组容器库
#include <cstdint>                         // C++ 标志整形定义库uint_8
using namespace cv;                        // 简化 cv::Mat 书写

typedef struct
{
    MppCodingType type;    // 编码类型、如MPP_VIDEO_CodingAVC(H.264),MPP_VIDEO_HEVC(H.265)
    RK_U32 width;          // 视频帧的宽度，需要按十六字节对齐(MPP强制要求)
    RK_U32 height;         // 视频帧的高度，需要按十六字节对齐
    MppFrameFormat format; // 视频帧的输入格式，MPP_FMT_YUV420
    RK_U32 num_frames;     // 计划编码的总帧数
} MpiEncTestCmd; // 编码的一些设置

typedef struct
{
    /*--------流控制标志--------*/
    RK_U32 frm_eos;     // 输入帧结束标志，置1表示没有更多的帧输入
    RK_U32 pkt_eos;     // 输出包结束的标志，置1表示编码器已经输出最后的数据包
    RK_U32 frame_count; // 已经编码的帧数的计数器
    RK_U32 stream_size; // 已经编码的数据总大小

    /*-------输入/输出文件----------*/
    FILE *fp_input;          // 输入文件指针(输入的原始YUV)
    FILE *fp_output;         // 输出文件指针(输出编码后的H.264)

    /*--------编码资源---------*/
    MppBuffer frm_buf;      // 帧缓冲区，等待存储的原始帧（MppBuffer 是 MPP 的缓冲区句柄）
    MppEncSeiMode sei_mode; // SEI帧模式，如 MPP_ENC_SEI_MODE_ONE_FRAME 表示每帧携带 SEI。

    /*------MPP 基础上下文---------*/
    MppCtx ctx;                // MPP 上下文句柄，表示一个编解码实例对象
    MppApi *mpi;               // MPP 接口函数表，包含编码、解码控制帧等 api 接口
    MppEncPrepCfg prep_cfg;    // 输入控制配置结构体，编码预处理结构体配置，输入图像的宽度、高度、格式等配置
    MppEncRcCfg rc_cfg;        // 码率控制配置结构体，比如比特率、帧率
    MppEncCodecCfg condec_cfg; // 协议控制配置结构体，具体编码的参数配置

    /*--------编码参数-----------*/
    RK_U32 width;       // 图像高度
    RK_U32 height;      // 图像宽度
    RK_U32 hor_stride;  // 水平 stride ，一行像素占用字节数，通常 16 字节对齐
    RK_U32 ver_stride;  // 垂直 stride ，垂直行数，通常 16 字节对齐
    MppFrameFormat fmt; // 输入帧格式，NV12，YUV
    MppCodingType type; // 编码类型，H.264,H.265
    RK_U32 num_frames;  // 计划编码的总帧数（0 表示无限编码）

    /*----------资源与缓冲区---------*/
    size_t frame_size;  // 单帧数据大小（计算方式：hor_stride * ver_stride * 3 / 2，YUV420P 格式）。
    size_t packet_size; // 输出数据包缓冲区大小（通常 ≥ width * height，防止溢出）。

    /*---------码率控制运行时参数--------*/
    RK_S32 gop; // GOP 长度, I 帧的间隔长度，比如 30 代表每 30 帧 传递一个 I 帧
    RK_S32 fps; // 帧率
    RK_S32 bps; // 目标码率
} MpiEncTestData; // 定义 MpiEncTestData 结构体，包含 MPP 编码测试所需的完整参数和运行时状态

// 静态全局指针，指向 MPP 接口函数表，初始为 NULL
static MppApi *mpi = NULL;
// 静态全局 MPP 上下文句柄实例对象，初始为 NULL
static MppCtx ctx = NULL;
// 外部申明的原子布尔变量，标记是否处理完第一帧
extern std::atomic<bool> first_frame_flg;

struct H264Frame
{
    // 存储一个 NALU（Network Abstraction Layer Unit）网络抽象层的完整数据（包含起始码）
    std::vector<std::uint8_t> data;
    size_t startSize{};             // 起始码的长度（可能是 3 或 4 字节，例如 0x000001 或 0x00000001）
};

// 简单的 MPP 编码器封装类
class MppEncoder
{
public:
// 获取 MppEncoder 类的单例实例（静态方法）
static MppEncoder &instance();
// 将一帧 YUV420 图像编码为 H264 数据
MPP_RET encode(int width, int height, Mat yuv_frame,
                   char *(&encode_buf), size_t &encode_length);

private:
// 私有构造函数，禁止外部直接创建对象，配合单例模式使用
MppEncoder() = default;
};

// 函数声明：打印 MppPacket 中包含的所有 NALU（用于调试），参数 packet 是 MPP 封装的编码输出包
void print_nalus_in_packet(MppPacket packet);
// 函数声明：将 YUV 帧编码为 H.264 数据（与 MppEncoder::encode 功能类似）
void YuvtoH264(int width, int height, Mat yuv_frame, char *(&encode_buf), size_t &encode_length);
// 函数声明：将完整的 H.264 数据包（可能包含多个 NALU）拆分成独立的 NALU 单元
//   packet: 原始 H.264 数据缓冲区指针
//   packetSize: 数据大小
// 返回值：H264Frame 对象的 vector，每个元素代表一个完整的 NALU
std::vector<H264Frame> splitNalus(const char *packet, int packetSize);
// 结束头文件编译
#endif
