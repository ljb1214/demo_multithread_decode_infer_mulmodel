/*
 * GB28181 配置集中管理
 * 支持从 gb28181.conf 读取，不存在时使用内置默认值
 */

#ifndef PROJECT2_CONFIG_H
#define PROJECT2_CONFIG_H

#include <string>

struct GBConfig
{
    std::string serveIp;       // 平台 IP
    std::string servePort;     // 平台端口
    std::string serveName;     // 平台国标 ID
    std::string servePassword; // 平台密码

    std::string deviceIp;      // 本设备 IP
    std::string devicePort;    // 本设备 SIP 端口
    std::string deviceName;    // 本设备国标 ID

    std::string pushPort;      // 默认推流端口（本地发送 PS/H264 的端口）
};

// 获取全局 GB28181 配置（懒加载，线程安全够用）
const GBConfig &get_gb_config();

#endif // PROJECT2_CONFIG_H

