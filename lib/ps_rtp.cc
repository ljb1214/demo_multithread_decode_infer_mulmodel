
// 包含自定义的 PS/RTP 封装头文件
#include "ps_rtp.h"

/*** *@remark:	将传入的数据按位一个一个地写入比特流缓冲区
*@param :  buffer	[in]  比特流缓冲区结构体指针
*		   count	[in]  需要写入的位数
*		   bits 	[in]  要写入的数值（低位对齐）
*/
void bits_write(bits_buffer_s *buffer, int count, uint64_t bits)
{
    // 从最高位开始依次写入 count 位
    while (count > 0)
    {
        count--;
        // 检查当前位的值（从 bits 的最高位开始）
        if ((bits >> count) & 0x01)
        {
            // 当前位为 1：设置缓冲区当前字节的当前位为 1
            buffer->p_data[buffer->i_data] |= buffer->i_mask;
        }
        else
        {
            // 当前位为 0：清除当前位
            buffer->p_data[buffer->i_data] &= ~(buffer->i_mask);
        }
        // 掩码右移一位，准备写入下一个比特位
        buffer->i_mask >>= 1;
        // 如果当前字节的所有位都已填满，则移动到下一个字节，并重置掩码为最高位
        if (buffer->i_mask == 0)
        {
            buffer->i_data++;
            buffer->i_mask = 0x80; // 下一个字节的 MSB
        }
    }
}

// PS 头的封装
/*** *@remark:	PS 头的封装，填充具体字段（参考标准 ISO/IEC 13818-1）
*@param :	pData  [in] 存储 PS 头数据的缓冲区指针
*			s64Src [in] 系统时钟参考（SCR）值
*@return:	0 success, others failed
*/
int gb28181_make_ps_header(char *pData, unsigned long long s64Scr)
{
    bits_buffer_s bitsBuffer;
    unsigned long long lScrExt = (s64Scr) % 100; // SCR 扩展部分（90 kHz 时钟下的低 9 位）
    bitsBuffer.i_size = PS_HDR_LEN;              // PS 头长度（常量，通常为 14 字节）
    bitsBuffer.i_data = 0;                       // 当前字节索引
    bitsBuffer.i_mask = 0x80;                    // 掩码，从最高位开始写入
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PS_HDR_LEN);    // 清零缓冲区

    // 开始写入 PS 头字段
    // 起始码 0x000001BA（32 位）
    bits_write(&bitsBuffer, 32, 0x000001BA);
    // 标志位：marker bit (固定为 1) 和 SCR 的高 3 位等
    bits_write(&bitsBuffer, 2, 1);               // '01' (固定)
    bits_write(&bitsBuffer, 3, (s64Scr >> 30) & 0x07); // SCR 的 bits 32-30
    bits_write(&bitsBuffer, 1, 1);               // marker bit
    // SCR 的 bits 29-15 (15 位)
    bits_write(&bitsBuffer, 15, (s64Scr >> 15) & 0x7FFF);
    bits_write(&bitsBuffer, 1, 1);               // marker bit
    // SCR 的 bits 14-0 (15 位)
    bits_write(&bitsBuffer, 15, s64Scr & 0x7fff);
    bits_write(&bitsBuffer, 1, 1);               // marker bit
    // SCR 扩展（9 位）
    bits_write(&bitsBuffer, 9, lScrExt & 0x01ff);
    bits_write(&bitsBuffer, 1, 1);               // marker bit
    // 程序复用速率（program_mux_rate），固定为 255（GB28181 默认）
    bits_write(&bitsBuffer, 22, (255) & 0x3fffff);
    bits_write(&bitsBuffer, 2, 3);               // marker bits '11'
    bits_write(&bitsBuffer, 5, 0x1f);            // reserved (5 位全 1)
    bits_write(&bitsBuffer, 3, 0);               // 填充字节（stuffing length）
    return 0;
}

// 系统头（System Header）封装
int gb28181_make_sys_header(char *pData)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = SYS_HDR_LEN;             // 系统头长度（常量）
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, SYS_HDR_LEN);
    // 起始码 0x000001BB
    bits_write(&bitsBuffer, 32, 0x000001BB);
    // header_length: 系统头长度减去起始码和长度字段自身（6 字节）
    bits_write(&bitsBuffer, 16, SYS_HDR_LEN - 6);
    // rate_bound: 最大比特率（单位为 50 bytes/s），50000 对应约 20 Mbps
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 22, 50000);
    bits_write(&bitsBuffer, 1, 1);
    // audio_bound: 同时处理的音频流最大数量
    bits_write(&bitsBuffer, 6, 1);
    // fixed_flag: 0 = 可变比特率
    bits_write(&bitsBuffer, 1, 0);
    // CSPS_flag: 1 = 启用循环播放和 SCR 同步
    bits_write(&bitsBuffer, 1, 1);
    // audio_lock_flag: 1 = 音频解码跟随系统时钟
    bits_write(&bitsBuffer, 1, 1);
    // video_lock_flag: 1 = 视频解码跟随系统时钟
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 1, 1);                // marker bit
    // video_bound: 视频流数量上限
    bits_write(&bitsBuffer, 5, 1);
    // 保留字段：bit 31-25，设 0 表示不启用 MPEG-1 之外的特性
    bits_write(&bitsBuffer, 1, 0);
    bits_write(&bitsBuffer, 7, 0x7F);              // reserved
    // 音频流绑定信息（stream_id 0xC0）
    bits_write(&bitsBuffer, 8, 0xC0);
    bits_write(&bitsBuffer, 2, 3);                 // marker bits
    bits_write(&bitsBuffer, 1, 0);                 // P-STD_buffer_bound_scale (0 = 128 字节单位)
    bits_write(&bitsBuffer, 13, 512);              // P-STD_buffer_size_bound (512 * 128 = 64 KB)
    // 视频流绑定信息（stream_id 0xE0）
    bits_write(&bitsBuffer, 8, 0xE0);
    bits_write(&bitsBuffer, 2, 3);                 // marker bits
    bits_write(&bitsBuffer, 1, 1);                 // P-STD_buffer_bound_scale (1 = 1024 字节单位)
    bits_write(&bitsBuffer, 13, 2048);             // P-STD_buffer_size_bound (2048 * 1024 = 2 MB)
    return 0;
}

/*** *@remark:	PSM（Program Stream Map）头的封装
*@param :	pData  [in] 存储 PSM 头数据的缓冲区指针
*@return:	0 success, others failed
*/
int gb28181_make_psm_header(char *pData)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = PSM_HDR_LEN;               // PSM 头长度
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PSM_HDR_LEN);
    // 起始码 0x000001BC
    bits_write(&bitsBuffer, 24, 0x000001);
    bits_write(&bitsBuffer, 8, 0xBC);              // map_stream_id = 0xBC
    bits_write(&bitsBuffer, 16, 18);               // program_stream_map_length (PSM 剩余长度)
    bits_write(&bitsBuffer, 1, 1);                 // current_next_indicator (1 = 当前有效)
    bits_write(&bitsBuffer, 2, 3);                 // reserved
    bits_write(&bitsBuffer, 5, 0);                 // program_stream_map_version (版本号)
    bits_write(&bitsBuffer, 7, 0x7F);              // reserved
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    bits_write(&bitsBuffer, 16, 0);                // programe_stream_info_length (无节目描述信息)
    bits_write(&bitsBuffer, 16, 8);                // elementary_stream_map_length (基本流映射长度，2 个流各 4 字节)
    // 音频流条目
    bits_write(&bitsBuffer, 8, 0x90);              // stream_type (音频)
    bits_write(&bitsBuffer, 8, 0xC0);              // elementary_stream_id (音频)
    bits_write(&bitsBuffer, 16, 0);                // elementary_stream_info_length
    // 视频流条目
    bits_write(&bitsBuffer, 8, 0x1B);              // stream_type (H.264)
    bits_write(&bitsBuffer, 8, 0xE0);              // elementary_stream_id (视频)
    bits_write(&bitsBuffer, 16, 0);                // elementary_stream_info_length
    // CRC_32 校验码（预计算值，此处固定）
    bits_write(&bitsBuffer, 8, 0x45);              // CRC 字节 3
    bits_write(&bitsBuffer, 8, 0xBD);              // CRC 字节 2
    bits_write(&bitsBuffer, 8, 0xDC);              // CRC 字节 1
    bits_write(&bitsBuffer, 8, 0xF4);              // CRC 字节 0
    return 0;
}

/*** *@remark:	PES 头的封装
*@param :	pData	   [in] 存储 PES 头的缓冲区
*			stream_id  [in] 流标识（0xE0 视频，0xC0 音频）
*			paylaod_len[in] 负载长度（PES 包中有效数据长度）
*			pts 	   [in] 显示时间戳
*			dts 	   [in] 解码时间戳
*@return:	0 success, others failed
*/
int gb28181_make_pes_header(char *pData, int stream_id, int payload_len, unsigned long long pts, unsigned long long dts)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = PES_HDR_LEN;               // PES 头长度（常量，通常为 19 字节）
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PES_HDR_LEN);
    // PES 起始码 0x000001
    bits_write(&bitsBuffer, 24, 0x000001);
    bits_write(&bitsBuffer, 8, (stream_id));       // stream_id
    // packet_len = 负载长度 + PES 头中除起始码和 packet_len 字段之外的字节数（13 字节）
    bits_write(&bitsBuffer, 16, (payload_len) + 13);
    bits_write(&bitsBuffer, 2, 2);                 // '10' (固定)
    bits_write(&bitsBuffer, 2, 0);                 // scrambling_control (不加密)
    bits_write(&bitsBuffer, 1, 0);                 // priority (低优先级)
    bits_write(&bitsBuffer, 1, 0);                 // data_alignment_indicator (不对齐)
    bits_write(&bitsBuffer, 1, 0);                 // copyright
    bits_write(&bitsBuffer, 1, 0);                 // original_or_copy
    bits_write(&bitsBuffer, 1, 1);                 // PTS_flag (有 PTS)
    bits_write(&bitsBuffer, 1, 1);                 // DTS_flag (有 DTS)
    bits_write(&bitsBuffer, 1, 0);                 // ESCR_flag
    bits_write(&bitsBuffer, 1, 0);                 // ES_rate_flag
    bits_write(&bitsBuffer, 1, 0);                 // DSM_trick_mode_flag
    bits_write(&bitsBuffer, 1, 0);                 // additional_copy_info_flag
    bits_write(&bitsBuffer, 1, 0);                 // PES_CRC_flag
    bits_write(&bitsBuffer, 1, 0);                 // PES_extension_flag
    bits_write(&bitsBuffer, 8, 10);                // header_data_length (PES 头中可选字段的长度，此处为 10 字节)
    // PTS 编码
    bits_write(&bitsBuffer, 4, 3);                 // '0011' (PTS 标识)
    bits_write(&bitsBuffer, 3, ((pts) >> 30) & 0x07); // PTS[32..30]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    bits_write(&bitsBuffer, 15, ((pts) >> 15) & 0x7FFF); // PTS[29..15]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    bits_write(&bitsBuffer, 15, (pts) & 0x7FFF);   // PTS[14..0]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    // DTS 编码
    bits_write(&bitsBuffer, 4, 1);                 // '0001' (DTS 标识)
    bits_write(&bitsBuffer, 3, ((dts) >> 30) & 0x07); // DTS[32..30]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    bits_write(&bitsBuffer, 15, ((dts) >> 15) & 0x7FFF); // DTS[29..15]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    bits_write(&bitsBuffer, 15, (dts) & 0x7FFF);   // DTS[14..0]
    bits_write(&bitsBuffer, 1, 1);                 // marker bit
    return 0;
}

// 检测给定的缓冲区中是否有 H.264 起始码（00 00 01 或 00 00 00 01）
int findStartCode(char *buf, int zeros_in_startcode)
{
    int info = 1; // 假设是起始码
    int i;
    // 检查前 zeros_in_startcode 个字节是否为 0
    for (i = 0; i < zeros_in_startcode; i++)
        if (buf[i] != 0)
            info = 0;
    // 检查第 zeros_in_startcode 个字节是否为 1
    if (buf[i] != 1)
        info = 0;
    return info; // 返回 1 表示是起始码
}

/**
 * 从输入文件中读取下一个 NAL 单元（未在此文件中实现，仅为注释）
 * @param inpf 文件指针
 * @param buf 存储 NAL 单元的缓冲区
 * @return NAL 单元的实际长度（不含起始码），或 0 若 EOF
 */

// RTP 头封装
/**
 * 打包 RTP 头
 * @param pData 指向 RTP 头缓冲区的指针
 * @param marker_flag RTP 中的 marker 位
 * @param cseq RTP 序列号
 * @param curpts RTP 时间戳
 * @param ssrc RTP 同步源标识
 * @return 0 成功，其他失败
 */
int gb28181_make_rtp_header(char *pData, int marker_flag, unsigned short cseq, long long curpts, unsigned int ssrc)
{
    bits_buffer_s bitsBuffer;
    if (pData == NULL)
        return -1;
    bitsBuffer.i_size = RTP_HDR_LEN;              // RTP 头长度（12 字节）
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, RTP_HDR_LEN);
    bits_write(&bitsBuffer, 2, RTP_VERSION);      // 版本号（2）
    bits_write(&bitsBuffer, 1, 0);                // 填充位
    bits_write(&bitsBuffer, 1, 0);                // 扩展位
    bits_write(&bitsBuffer, 4, 0);                // CSRC 计数
    bits_write(&bitsBuffer, 1, (marker_flag));    // marker 位
    bits_write(&bitsBuffer, 7, 96);               // payload type（96，H.264）
    bits_write(&bitsBuffer, 16, (cseq));          // 序列号
    bits_write(&bitsBuffer, 32, (curpts));        // 时间戳
    bits_write(&bitsBuffer, 32, (ssrc));          // SSRC
    return 0;
}

/**
 * 将 H.264 数据封装为 PS + RTP 包并发送（普通版本）
 * @param pData H.264 码流数据（不含起始码）
 * @param nFrameLen 数据长度
 * @param pPacker RTP 打包上下文
 * @param stream_type 流类型（0：视频，1：音频）
 * @param __cp 目标 IP 地址
 * @param __hostshort 目标端口
 * @param udp_socket UDP 套接字
 * @return 0 成功，-1 失败
 */
int gb28181_streampackageForH264(char *pData, int nFrameLen, Data_Info_s *pPacker, int stream_type, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    int is_keyframe = 0;
    char szTempPacketHead[256]; // 临时存放头部数据
    int nSizePos = 0;
    int nSize = 0;              // 当前分片的有效载荷长度
    char *pBuff = NULL;
    memset(szTempPacketHead, 0, 256);
    // 1、PS 头
    gb28181_make_ps_header(szTempPacketHead + nSizePos, pPacker->s64CurPts);
    nSizePos += PS_HDR_LEN;

    // 判断当前帧是否为关键帧（通过 NAL 类型）
    if (nFrameLen >= 4)
    {
        uint8_t *nal_start = (uint8_t *)pData;
        if (nal_start[0] == 0x00 && nal_start[1] == 0x00 &&
            (nal_start[2] == 0x01 || (nal_start[2] == 0x00 && nal_start[3] == 0x01)))
        {
            uint8_t nal_type = nal_start[2] == 0x01 ? nal_start[3] & 0x1f : nal_start[4] & 0x1f;
            if (nal_type == 0x05)               // IDR 帧
            {
                is_keyframe = 1;
            }
        }
    }
    // 如果是关键帧，添加系统头和 PSM 头
    if (is_keyframe == 1)
    {
        gb28181_make_sys_header(szTempPacketHead + nSizePos);
        nSizePos += SYS_HDR_LEN;
        gb28181_make_psm_header(szTempPacketHead + nSizePos);
        nSizePos += PSM_HDR_LEN;
    }
    // 发送第一个 RTP 包（只包含头部，无负载，mark_flag=0）
    if (gb28181_send_rtp_pack(szTempPacketHead, nSizePos, 0, pPacker, __cp, __hostshort, udp_socket) != 0)
        return -1;

    // PES 包处理：为每一块数据添加 PES 头
    pBuff = pData - PES_HDR_LEN; // 在数据前预留 PES 头空间（零拷贝）
    while (nFrameLen > 0)
    {
        if (nFrameLen > PS_PES_PAYLOAD_SIZE)   // 大于最大负载长度
            nSize = PS_PES_PAYLOAD_SIZE;       // 分片
        else
            nSize = nFrameLen;                 // 最后一片

        // 添加 PES 头
        gb28181_make_pes_header(pBuff, stream_type ? 0xC0 : 0xE0, nSize, pPacker->s64CurPts, pPacker->s64CurPts);
        // 发送 RTP 包（负载为 PES 头 + 数据），最后一个包标记 marker=1
        if (gb28181_send_rtp_pack(pBuff, nSize + PES_HDR_LEN, ((nSize == nFrameLen) ? 1 : 0), pPacker, __cp, __hostshort, udp_socket))
        {
            printf("gb28181_send_pack failed!\n");
            return -1;
        }
        nFrameLen -= nSize;
        pBuff += nSize; // 指针后移
    }
    return 0;
}

/**
 * 发送第一帧（包含 SPS/PPS/IDR 的特殊处理）
 * @param data H.264 帧向量（顺序：SPS, PPS, IDR）
 * @param pPacker RTP 打包上下文
 * @param stream_type 流类型
 * @param __cp 目标 IP
 * @param __hostshort 目标端口
 * @param udp_socket UDP 套接字
 * @return 0 成功，负数失败
 */
int gb28181_streampackageForH264_first(std::vector<H264Frame> data, struct Data_Info_s *pPacker,
                                       int stream_type, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    // 1. 参数校验
    if (data.size() < 3 || !pPacker || !__cp)
    {
        return -1; // 参数无效
    }

    // 2. 计算各部分大小
    const int total_header_size = PS_HDR_LEN + SYS_HDR_LEN + PSM_HDR_LEN;
    const int pes_header_size = PES_HDR_LEN;
    const int max_payload = RTP_MAX_PACKET_BUFF - RTP_HDR_LEN - total_header_size; // 有效负载最大长度

    // 3. 分配缓冲区
    std::vector<char> packet_buffer(RTP_MAX_PACKET_BUFF, 0);
    char *pBuff = packet_buffer.data();

    // 4. 构建 PS 头、系统头、PSM 头
    gb28181_make_ps_header(pBuff, pPacker->s64CurPts);
    gb28181_make_sys_header(pBuff + PS_HDR_LEN);
    gb28181_make_psm_header(pBuff + PS_HDR_LEN + SYS_HDR_LEN);

    int offset = total_header_size; // 当前写入位置

    // 5. 处理 SPS
    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                static_cast<int>(data[0].data.size()),
                                pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2; // PES 头生成失败
    }
    offset += pes_header_size;
    memcpy(pBuff + offset, data[0].data.data(), data[0].data.size());
    offset += static_cast<int>(data[0].data.size());

    // 6. 处理 PPS
    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                static_cast<int>(data[1].data.size()),
                                pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2;
    }
    offset += pes_header_size;
    memcpy(pBuff + offset, data[1].data.data(), data[1].data.size());
    offset += static_cast<int>(data[1].data.size());

    // 7. 处理 IDR 帧的第一部分
    int remaining_idr_size = max_payload - (offset - total_header_size) - pes_header_size;
    if (remaining_idr_size <= 0 || remaining_idr_size > static_cast<int>(data[2].data.size()))
    {
        return -3; // 缓冲区空间不足
    }
    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                remaining_idr_size, pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2;
    }
    offset += pes_header_size;
    memcpy(pBuff + offset, data[2].data.data(), remaining_idr_size);
    offset += remaining_idr_size;

    // 8. 发送第一个 RTP 包（包含所有头部和部分 IDR 数据）
    if (gb28181_send_rtp_pack(pBuff, offset, 0, pPacker, __cp, __hostshort, udp_socket) < 0)
    {
        return -4; // 发送失败
    }
    printf("send first frame\n");

    // 9. 处理剩余 IDR 数据
    int remaining_data = static_cast<int>(data[2].data.size()) - remaining_idr_size;
    char *pRemainingData = reinterpret_cast<char *>(data[2].data.data()) + remaining_idr_size;

    while (remaining_data > 0)
    {
        int chunk_size = (remaining_data > PS_PES_PAYLOAD_SIZE) ? PS_PES_PAYLOAD_SIZE : remaining_data;
        if (gb28181_send_rtp_pack(pRemainingData, chunk_size, ((chunk_size == remaining_data) ? 1 : 0),
                                  pPacker, __cp, __hostshort, udp_socket) < 0)
        {
            return -4;
        }
        pRemainingData += chunk_size;
        remaining_data -= chunk_size;
    }
    return 0;
}

// 发送数据包（底层 UDP 发送）
#include <unistd.h>
#include <poll.h>
int SendDataBuff(char *buff, int size, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    struct sockaddr_in addr_serv;
    memset(&addr_serv, 0, sizeof(addr_serv));
    addr_serv.sin_family = AF_INET;
    addr_serv.sin_addr.s_addr = inet_addr(__cp); // 目标 IP
    addr_serv.sin_port = htons(__hostshort);     // 目标端口
    socklen_t len = sizeof(addr_serv);

    struct pollfd fds;
    fds.fd = udp_socket;
    fds.events = POLLOUT; // 监听可写事件

    int max_retries = 3;
    for (int attempt = 1; attempt <= max_retries; attempt++)
    {
        int poll_result = poll(&fds, 1, 100); // 等待 100ms
        if (poll_result > 0)                 // 可写
        {
            int ret = sendto(udp_socket, buff, size, 0, (struct sockaddr *)&addr_serv, len);
            if (ret > 0)
                return ret;
            else
                perror("[ERROR] sendto failed");
        }
        else if (poll_result == 0)
        {
            printf("[INFO] Timeout, retrying...\n");
        }
        else
        {
            perror("[ERROR] poll failed");
            return -1;
        }
        printf("[INFO] Retrying %d/%d...\n", attempt, max_retries);
    }
    printf("[ERROR] Send failed after %d retries\n", max_retries);
    return -1;
}

/**
 * 发送 RTP 包
 * @param databuff 指向负载数据的缓冲区（不含 RTP 头）
 * @param nDataLen 负载数据长度
 * @param mark_flag 是否标记为最后一包
 * @param pPacker RTP 上下文
 * @param __cp 目标 IP
 * @param __hostshort 目标端口
 * @param udp_socket UDP 套接字
 * @return 0 成功，-1 失败
 */
int gb28181_send_rtp_pack(char *databuff, int nDataLen, int mark_flag, Data_Info_s *pPacker, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    // 可选：将原始数据保存到文件（用于调试）
    FILE *fp = fopen("output.ps", "ab");
    if (fp)
    {
        fwrite(databuff, 1, nDataLen, fp);
        fclose(fp);
    }

    int nRes = 0;
    int nPlayLoadLen = 0;
    int nSendSize = 0;
    char szRtpHdr[RTP_HDR_LEN];
    memset(szRtpHdr, 0, RTP_HDR_LEN);

    // 如果总长度（RTP 头 + 负载）不超过最大包大小，则一包发送
    if (nDataLen + RTP_HDR_LEN <= RTP_MAX_PACKET_BUFF)
    {
        // 构建 RTP 头
        gb28181_make_rtp_header(szRtpHdr, ((mark_flag == 1) ? 1 : 0), ++pPacker->u16CSeq,
                                pPacker->s64CurPts, pPacker->u32Ssrc);
        // 将 RTP 头和负载拷贝到发送缓冲区
        memcpy(pPacker->szBuff, szRtpHdr, RTP_HDR_LEN);
        memcpy(pPacker->szBuff + RTP_HDR_LEN, databuff, nDataLen);
        nRes = SendDataBuff(pPacker->szBuff, nDataLen + RTP_HDR_LEN, __cp, __hostshort, udp_socket);
        if (nRes != (RTP_HDR_LEN + nDataLen))
        {
            printf(" udp send error2 !\n");
            return -1;
        }
    }
    else
    {
        // 分片发送
        nPlayLoadLen = RTP_MAX_PACKET_BUFF - RTP_HDR_LEN; // 每个包最大负载
        // 第一片
        gb28181_make_rtp_header(pPacker->szBuff, 0, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
        memcpy(pPacker->szBuff + RTP_HDR_LEN, databuff, nPlayLoadLen);
        nRes = SendDataBuff(pPacker->szBuff, RTP_HDR_LEN + nPlayLoadLen, __cp, __hostshort, udp_socket);
        if (nRes != (RTP_HDR_LEN + nPlayLoadLen))
        {
            printf(" udp send error1 !\n");
            return -1;
        }
        nDataLen -= nPlayLoadLen;
        databuff += nPlayLoadLen;          // 指针移到剩余数据
        databuff -= RTP_HDR_LEN;           // 为 RTP 头预留空间（回退）
        // 剩余数据分片发送
        while (nDataLen > 0)
        {
            if (nDataLen <= nPlayLoadLen)
            {
                // 最后一片，设置 marker
                gb28181_make_rtp_header(databuff, mark_flag, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
                nSendSize = nDataLen;
            }
            else
            {
                gb28181_make_rtp_header(databuff, 0, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
                nSendSize = nPlayLoadLen;
            }
            nRes = SendDataBuff(databuff, RTP_HDR_LEN + nSendSize, __cp, __hostshort, udp_socket);
            if (nRes != (RTP_HDR_LEN + nSendSize))
            {
                printf(" udp send error3 !\n");
                return -1;
            }
            nDataLen -= nSendSize;
            databuff += nSendSize;
        }
    }
    return 0;
}