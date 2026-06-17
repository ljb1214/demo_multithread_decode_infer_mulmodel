#ifndef _GB28181_H                // 头文件保护宏：防止重复包含
#define _GB28181_H                // 定义宏，表示该头文件已被包含

#include <iostream>               // 包含输入输出流库，用于标准输入输出
#include "eXosip2/eXosip.h"       // 包含eXosip库头文件，用于SIP协议栈
#include <netinet/in.h>           // 包含网络地址结构定义，如sockaddr_in
#include <thread>                 // 包含线程库，用于多线程操作
#include <vector>                 // 包含向量容器，用于存储动态数组
#include <sstream>                // 包含字符串流，用于字符串与数据转换
#include <map>                    // 包含映射容器，用于键值对存储
#include "xml_utils.h"            // 包含自定义的XML工具头文件，用于解析和生成XML
#include <map>                    // 再次包含map，重复但无影响
#include <string>                 // 包含字符串类
#include <vector>                 // 再次包含vector，重复
using std::map;                   // 使用标准命名空间中的map，简化代码
using std::string;                // 使用标准命名空间中的string
using std::vector;                // 使用标准命名空间中的vector

// 媒体流结构体，描述一个媒体流的信息
struct MediaStream
{
    string mediaType;             // 媒体类型，如"video"或"audio"
    string port;                  // 媒体流使用的端口号（字符串形式）
    string protocol;              // 传输协议，如"RTP/AVP"
    string trans;                 // 传输方式，如"TCP"或"UDP"
    vector<string> payloadTypes;  // 负载类型列表，如"96", "97"等
    map<string, string> rtpmaps;  // RTP映射表，键为负载类型，值为编码名称/时钟频率等
    vector<string> core;          // 核心信息，可能用于扩展
};

// 连接信息结构体，描述网络地址信息
struct connectionInfo
{
    string ip;                    // IP地址
    string networkType;           // 网络类型，如"IN"
    string addressType;           // 地址类型，如"IP4"或"IP6"
};

// 会话信息结构体，对应SDP中的Origin字段
struct SessionInfo
{
    string username;              // 用户名
    string sessionID;             // 会话ID
    string sessionVersion;        // 会话版本号
    string networkType;           // 网络类型
    string addressType;           // 地址类型
    string connectionAddress;     // 连接地址
};

// SDP信息结构体，汇总所有SDP相关字段
struct SDPInfo
{
    string version;               // SDP版本号，通常是"0"
    SessionInfo origin;           // 会话发起者信息（o=字段）
    string sessionName;           // 会话名称（s=字段）
    connectionInfo connectionAddress; // 连接信息（c=字段）
    string timing;                // 时间描述（t=字段）
    MediaStream mediaStreams;     // 媒体流描述（m=字段及相关属性）
    string ssrc;                  // 同步源标识（SSRC）
};

// GB28181配置信息结构体，保存SIP服务器和设备的参数
struct GB28Info
{
    std::string serveName;        // SIP服务器名称
    std::string serveIp;          // SIP服务器IP地址
    std::string servePort;        // SIP服务器端口
    std::string deviceName;       // 设备名称（通常为设备ID）
    std::string deviceIp;         // 设备IP地址
    std::string devicePort;       // 设备端口
    std::string servePassword;    // 服务器认证密码
    std::string pushPort;         // 推流端口（用于媒体传输）
};

// GB28181连接类，封装与GB28181平台的交互功能
class GB28181Connect
{
public:
    // 显式构造函数，接收GB28Info配置参数，禁止隐式转换
    explicit GB28181Connect(const GB28Info &gbinfo);

    // 发送注册请求到SIP服务器
    int send_register(eXosip_t *ctx, GB28Info info);

    // 响应目录查询请求（通过MESSAGE方法）
    void respond_catalog_by_message(eXosip_t *ctx, int sn, const GB28Info &info);

    // 响应设备信息查询请求（通过MESSAGE方法）
    void respond_deviceinfo_by_message(eXosip_t *ctx, int sn, const GB28Info &info);

    // 响应设备控制请求（通过MESSAGE方法）
    void respond_deviceControl_by_message(eXosip_t *ctx, int sn, const GB28Info &info);

    // 响应移动位置订阅请求（通过SUBSCRIBE方法）
    void respond_MobilePosition_by_subscribe(eXosip_t *ctx, int sn, const GB28Info &info);

    // 发送目录列表（通道列表）作为响应
    void send_channel_list(eXosip_t *ctx, int sn, GB28Info info);

    // 处理注册事件（如REGISTER请求或响应）
    int handleRegister(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);

    // 处理NOTIFY事件（一般用于订阅通知）
    int handleNews(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);

    // 处理INVITE邀请请求（建立媒体会话），解析SDP信息并填充sdp_info
    int handleInvite(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo, SDPInfo &sdp_info);

    // 处理SUBSCRIBE订阅请求
    int handleSubcribe(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);

    GB28Info info;                // 保存当前连接的GB28181配置信息
    SDPInfo sdp_info;             // 保存当前会话的SDP信息
};

// 全局注册线程函数，用于定时刷新注册
void register_thread(int second, eXosip_t *ctx, eXosip_event_t *event, int rID);

#endif // _GB28181_H           // 结束头文件保护宏