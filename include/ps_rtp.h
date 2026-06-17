#ifndef PROJECT2_PS_RTP_H                // 头文件保护宏：如果未定义PROJECT2_PS_RTP_H，则定义它，防止重复包含
#define PROJECT2_PS_RTP_H                // 定义宏，表示头文件已被包含

#include <stdint.h>                      // 包含标准整数类型定义，如uint8_t、uint16_t等
#include <string>                        // 包含字符串类
#include <vector>                        // 包含向量容器
#include <sys/types.h>                   // 包含系统数据类型定义，如size_t、ssize_t等
#include <sys/socket.h>                  // 包含套接字相关函数和结构体
#include <netinet/in.h>                  // 包含网络地址结构体，如sockaddr_in
#include <arpa/inet.h>                   // 包含IP地址转换函数，如inet_pton
#include <sys/ioctl.h>                   // 包含I/O控制操作，如ioctl
#include <unistd.h>                      // 包含UNIX标准函数，如close、read等

#include "decode.h"                      // 包含自定义的解码头文件，可能包含H264Frame等定义

#define PS_HDR_LEN 14                    // PS（节目流）头长度，14字节
#define SYS_HDR_LEN 18                   // 系统头长度，18字节
#define PSM_HDR_LEN 24                   // 节目流映射（PSM）头长度，24字节
#define PES_HDR_LEN 19                   // PES（打包基本流）头长度，19字节
#define RTP_HDR_LEN 12                   // RTP（实时传输协议）头长度，12字节
#define RTP_VERSION 2                    // RTP协议版本号，固定为2
#define RTP_MAX_PACKET_BUFF 1400         // RTP最大包缓冲区大小，1400字节
#define PS_PES_PAYLOAD_SIZE 65522        // PS PES负载最大大小，65522字节（被注释，但保留）

// 比特缓冲区结构体，用于按位读写操作
struct bits_buffer_s
{
    unsigned char *p_data;               // 指向数据缓冲区的指针
    unsigned char i_mask;                // 掩码，用于位操作
    int i_size;                          // 缓冲区总大小（字节）
    int i_data;                          // 当前已填充的数据大小（字节）
};

// 数据信息结构体，封装RTP发送所需的参数
struct Data_Info_s
{
    uint64_t s64CurPts;                  // 当前PTS（呈现时间戳），64位无符号整数
    int IFrame;                          // 是否为I帧标志，1表示I帧，0表示非I帧
    uint16_t u16CSeq;                    // RTP序列号，16位无符号整数
    uint32_t u32Ssrc;                    // RTP同步源标识符，32位无符号整数
    char szBuff[RTP_MAX_PACKET_BUFF];    // 数据发送缓冲区，大小RTP_MAX_PACKET_BUFF
};

// 构建RTP头，填充到pData中
// pData: 输出缓冲区指针
// marker_flag: 标记位，通常用于表示一帧结束
// cseq: 序列号
// curpts: 当前时间戳
// ssrc: 同步源标识
int gb28181_make_rtp_header(char *pData, int marker_flag, unsigned short cseq, long long curpts, unsigned int ssrc);

// 构建PES头
// pData: 输出缓冲区指针
// stream_id: 流ID，如0xE0表示视频流
// payload_len: PES负载长度
// pts: 呈现时间戳
// dts: 解码时间戳
int gb28181_make_pes_header(char *pData, int stream_id, int payload_len, unsigned long long pts, unsigned long long dts);

// 构建PSM（节目流映射）头
// pData: 输出缓冲区指针
int gb28181_make_psm_header(char *pData);

// 构建系统头
// pData: 输出缓冲区指针
int gb28181_make_sys_header(char *pData);

// 发送RTP包
// databuff: 待发送数据缓冲区
// nDataLen: 数据长度
// mark_flag: RTP头标记位
// pPacker: 数据信息结构体，包含序列号、SSRC等
// dest_ip: 目标IP地址字符串
// dest_port: 目标端口号
// udp_socket: UDP套接字描述符
int gb28181_send_rtp_pack(char *databuff, int nDataLen, int mark_flag, Data_Info_s *pPacker, const char *dest_ip, uint16_t dest_port, int udp_socket);

// 构建PS头（节目流头）
// pData: 输出缓冲区指针
// s64Scr: 系统时钟参考（SCR）
int gb28181_make_ps_header(char *pData, unsigned long long s64Scr);

// 直接发送数据缓冲区
// buff: 待发送数据缓冲区
// size: 数据大小
// dest_ip: 目标IP地址字符串
// dest_port: 目标端口号
// udp_socket: UDP套接字描述符
int SendDataBuff(char *buff, int size, const char *dest_ip, uint16_t dest_port, int udp_socket);

// 在缓冲区中查找起始码（如00 00 01）
// buf: 缓冲区指针
// zeros_in_startcode: 起始码中0的个数（通常为2或3）
int findStartCode(char *buf, int zeros_in_startcode);

// 将H264帧打包为PS流并发送（用于普通帧）
// pData: H264数据缓冲区
// nFrameLen: 帧长度
// pPacker: 数据信息结构体
// stream_type: 流类型，如0x1B表示H.264
// dest_ip: 目标IP地址字符串
// dest_port: 目标端口号
// udp_socket: UDP套接字描述符
int gb28181_streampackageForH264(char *pData, int nFrameLen, Data_Info_s *pPacker, int stream_type, const char *dest_ip, uint16_t dest_port, int udp_socket);

// 将第一个H264帧打包为PS流并发送（可能用于处理序列头等）
// data: H264帧向量（可能是多个NALU）
// pPacker: 数据信息结构体
// stream_type: 流类型
// dest_ip: 目标IP地址字符串
// dest_port: 目标端口号
// udp_socket: UDP套接字描述符
int gb28181_streampackageForH264_first(std::vector<H264Frame> data, Data_Info_s *pPacker, int stream_type, const char *dest_ip, uint16_t dest_port, int udp_socket);

#endif /* PROJECT2_PS_RTP_H */          // 结束头文件保护宏