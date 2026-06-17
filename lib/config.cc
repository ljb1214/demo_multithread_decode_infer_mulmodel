/*
 * GB28181 配置集中管理
 * 功能：读取gb28181.conf配置文件 + 内置默认值，提供全局统一的GB28181配置获取接口
 */

#include "config.h"       // 包含GB28181配置结构体定义
#include "path_utils.h"   // 包含配置文件路径解析工具函数
#include <filesystem>    // C++17 文件系统操作库（用于路径处理）
#include <fstream>       // 文件输入流，用于读取配置文件
#include <sstream>       // 字符串流，用于字符串处理
#include <iostream>      // 标准输入输出，用于打印日志

// 匿名命名空间：内部函数，外部无法访问（仅当前文件可见）
namespace
{
// 字符串 trim 函数：去除字符串首尾的空格、制表符、换行、回车
void trim(std::string &s)
{
    // 定义要去除的空白字符集合
    const char *ws = " \t\r\n";
    // 找到字符串第一个**非空白字符**的位置
    auto start = s.find_first_not_of(ws);
    // 找到字符串最后一个**非空白字符**的位置
    auto end = s.find_last_not_of(ws);
    
    // 如果全是空白字符（找不到有效字符）
    if (start == std::string::npos)
    {
        // 清空字符串
        s.clear();
        // 直接返回
        return;
    }
    
    // 截取有效字符串（从 start 到 end）
    s = s.substr(start, end - start + 1);
}

// 从配置文件 gb28181.conf 加载配置到 cfg 结构体
void load_from_file(GBConfig &cfg)
{
    // 调用工具函数，解析配置文件完整路径（优先环境变量MYDEMO_GB_CONFIG，否则默认路径）
    const std::string config_path = resolve_config_path("gb28181.conf", "MYDEMO_GB_CONFIG");
    // 创建文件输入流，打开配置文件
    std::ifstream ifs(config_path);
    
    // 如果文件打开失败（文件不存在/无权限）
    if (!ifs.is_open())
    {
        // 打印日志：未找到配置文件，使用内置默认值
        std::cout << "[Config] gb28181.conf not found at " << config_path
                  << ", using built-in defaults." << std::endl;
        // 直接返回，不加载文件
        return;
    }

    // 打印日志：成功加载配置文件路径
    std::cout << "[Config] Loading gb28181.conf from " << config_path << std::endl;

    // 定义一行字符串，存储读取的每一行内容
    std::string line;
    // 逐行读取配置文件，直到文件结束
    while (std::getline(ifs, line))
    {
        // 去除当前行首尾空白字符
        trim(line);
        // 如果当前行为空 或者 以#开头（注释行），跳过不处理
        if (line.empty() || line[0] == '#')
            continue;

        // 查找等号 = 的位置（key=value 格式）
        auto pos = line.find('=');
        // 如果没有等号，格式错误，跳过
        if (pos == std::string::npos)
            continue;

        // 截取等号左边内容作为配置项 key
        std::string key = line.substr(0, pos);
        // 截取等号右边内容作为配置项 value
        std::string value = line.substr(pos + 1);
        // 去除 key 首尾空白
        trim(key);
        // 去除 value 首尾空白
        trim(value);

        // ===================== 匹配 key 并赋值给配置结构体 =====================
        // 服务端IP
        if (key == "serveIp")
            cfg.serveIp = value;
        // 服务端端口
        else if (key == "servePort")
            cfg.servePort = value;
        // 服务端ID/名称
        else if (key == "serveName")
            cfg.serveName = value;
        // 服务端密码
        else if (key == "servePassword")
            cfg.servePassword = value;
        // 设备端IP
        else if (key == "deviceIp")
            cfg.deviceIp = value;
        // 设备端端口
        else if (key == "devicePort")
            cfg.devicePort = value;
        // 设备ID/名称
        else if (key == "deviceName")
            cfg.deviceName = value;
        // 推流端口
        else if (key == "pushPort")
            cfg.pushPort = value;
    }
}
} // 匿名命名空间结束

// 全局获取 GB28181 配置的接口（单例模式，全局唯一）
const GBConfig &get_gb_config()
{
    // 静态配置结构体：程序运行期间只初始化一次，全局共享
    static GBConfig cfg;
    // 静态初始化标志：只执行一次初始化
    static bool initialized = false;

    // 如果还未初始化
    if (!initialized)
    {
        // ===================== 设置【内置默认配置】 =====================
        // 设备端口默认值
        cfg.devicePort = "15060";
        // 设备IP默认值
        cfg.deviceIp = "192.168.43.7";
        // 设备ID默认值
        cfg.deviceName = "32000000001320104389";

        // 服务端口默认值
        cfg.servePort = "5060";
        // 服务IP默认值
        cfg.serveIp = "192.168.43.5";
        // 服务ID默认值
        cfg.serveName = "34020000002000000001";
        // 服务密码默认值
        cfg.servePassword = "123456";

        // 推流端口默认值
        cfg.pushPort = "7000";

        // 从配置文件加载配置（文件配置会**覆盖**上面的默认值）
        load_from_file(cfg);
        // 标记已初始化，下次不再执行
        initialized = true;
    }

    // 返回全局唯一的配置结构体引用
    return cfg;
}