/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * 版权声明：版权归 HeXiaotian 所有
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 * 仅限非商业使用，禁止再分发、转售及衍生作品
 */

#ifndef PROJECT2_XML_UTILS_H                // 头文件保护宏：如果未定义 PROJECT2_XML_UTILS_H，则定义它，防止重复包含
#define PROJECT2_XML_UTILS_H                // 定义宏 PROJECT2_XML_UTILS_H，标识该头文件已被包含

#include <string>                           // 包含标准字符串类，用于 std::string

// 将 UTF-8 编码的字符串转换为 GB2312 编码
// inbuf: 输入 UTF-8 字符串
// outbuf: 输出 GB2312 字符串缓冲区
// outbufsize: 输出缓冲区大小
// 返回值: 成功返回转换后的字节数，失败返回 -1
int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize);

// 从 XML 字符串中提取 Session-Name（SN）字段的值
// xml: 包含 SN 的 XML 字符串
// 返回: SN 值（字符串）
std::string extractSN(const std::string &xml);

// 从 XML 消息体中提取命令类型（CmdType）字段的值
// xmlBody: XML 消息体字符串
// 返回: 命令类型字符串（如 "Catalog", "DeviceInfo" 等）
std::string extractCmdType(const char *xmlBody);

#endif /* PROJECT2_XML_UTILS_H */            // 结束头文件保护宏