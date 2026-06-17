/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

// 包含自定义的GB28181头文件，其中定义了GB28Info、SDPInfo等结构体
#include "gb28181.h"
// 包含标准算法库，用于transform等操作
#include <algorithm>
// 包含字符处理库，用于tolower等函数
#include <cctype>
// 包含C风格字符串操作库，如strlen等
#include <cstring>
// 包含互斥锁库，用于线程同步
#include <mutex>
// 包含字符串流库，用于解析字符串
#include <sstream>

// 全局互斥锁，用于保护对eXosip库的并发调用，因为eXosip不是线程安全的
std::mutex eXosip_mutex;

// 解析SDP（会话描述协议）字符串，将解析结果填充到info结构体中
int parseSDP(const std::string &sdp, SDPInfo &info)
{
    // 使用字符串输入流，以便按行读取
    std::istringstream iss(sdp);
    // 存储当前行的内容
    std::string line;
    // 临时媒体流对象，用于存储当前正在解析的媒体信息（但此变量未使用）
    MediaStream currentMedia;
    // 标志是否在媒体块内部（未使用）
    bool inMediaBlock = false;
    // 循环读取每一行
    while (getline(iss, line))
    {
        // 跳过空行
        if (line.empty())
            continue;
        // 查找等号的位置，SDP格式为 key=value
        size_t pos = line.find('=');
        // 如果没有等号，则不是有效SDP行，跳过
        if (pos == std::string::npos)
            continue;
        // 提取键（等号之前的部分）
        std::string key = line.substr(0, pos);
        // 提取值（等号之后的部分）
        std::string value = line.substr(pos + 1);
        // 根据键处理不同字段
        if (key == "v") // 版本号
        {
            info.version = value; // 保存版本号
        }
        else if (key == "o") // 源信息（Origin）
        {
            std::string tmp = value;
            // 使用字符串流按空格解析 o 字段
            std::istringstream is(tmp);
            // 依次解析：用户名、会话ID、会话版本、网络类型、地址类型、连接地址
            if (!(is >> info.origin.username >> info.origin.sessionID >> info.origin.sessionVersion >> info.origin.networkType >> info.origin.addressType >> info.origin.connectionAddress))
            {
                std::cout << "orin解析错误" << std::endl;
                return -3; // 解析失败返回错误码
            }
        }
        else if (key == "s") // 会话名称
        {
            info.sessionName = value;
        }
        else if (key == "c") // 连接信息（Connection）
        {
            std::string tmp = value;
            std::cout << "tmp:" << tmp << std::endl;
            // 按空格解析 c 字段：网络类型、地址类型、IP地址
            std::istringstream is(tmp);
            if (!(is >> info.connectionAddress.networkType >> info.connectionAddress.addressType >> info.connectionAddress.ip))
            {
                std::cout << "connectionAddress解析错误" << std::endl;
                return -1;
            }
        }
        else if (key == "t") // 时间信息（Timing）
        {
            info.timing = value;
        }
        else if (key == "m") // 媒体描述（Media）
        {
            std::istringstream mStream(value);
            // 解析媒体类型、端口、协议
            if (!(mStream >> info.mediaStreams.mediaType >> info.mediaStreams.port >> info.mediaStreams.protocol))
            {
                std::cout << "解析m失败" << std::endl;
                return -2;
            }
            std::string payload;
            // 继续读取剩余的负载类型号（payload types）
            while (mStream >> payload)
            {
                info.mediaStreams.payloadTypes.push_back(payload);
            }
        }
        else if (key == "a") // 属性（Attribute）
        {
            // 如果属性以 "rtpmap:" 开头，表示RTP映射
            if (value.find("rtpmap:") == 0)
            {
                // 格式: rtpmap:<payload> <codec>/<clockrate>
                size_t colonPos = value.find(':');   // 冒号位置
                size_t spacePos = value.find(' ');   // 空格位置
                size_t slashPos = value.find('/');   // 斜杠位置

                // 确保必要的分隔符都存在
                if (spacePos != std::string::npos && slashPos != std::string::npos)
                {
                    // 提取负载类型号
                    std::string payloadType = value.substr(colonPos + 1, spacePos - colonPos - 1);
                    // 提取编码名称
                    std::string codec = value.substr(spacePos + 1, slashPos - spacePos - 1);
                    // 提取时钟频率
                    std::string clockRate = value.substr(slashPos + 1);
                    // 将完整的 "codec/clockrate" 字符串存入rtpmaps映射中，键为payloadType
                    info.mediaStreams.rtpmaps[payloadType] = value.substr(spacePos + 1);
                }
            }
            // 如果是方向属性（只接收或只发送）
            else if (value == "recvonly" || value == "sendonly")
            {
                // 将方向信息保存到trans字段
                info.mediaStreams.trans = value;
            }
        }
        else if (key == "y") // GB28181扩展字段，用于SSRC
        {
            info.ssrc = value; // 保存SSRC
        }
    }
    // 解析成功
    return 0;
}

// GB28181连接类的构造函数，用给定的配置信息初始化成员
GB28181Connect::GB28181Connect(const GB28Info &gbinfo) : info(gbinfo)
{
    // 打印服务器相关信息
    std::cout << "服务器ID:" << this->info.serveName << std::endl;
    std::cout << "服务器ip:" << this->info.serveIp << std::endl;
    std::cout << "服务器密码:" << this->info.servePassword << std::endl;
    std::cout << "服务器端口:" << this->info.servePort << std::endl;
    // 注意：这里打印的是设备IP，但变量名是deviceIp，可能应该为deviceName，但按照原代码保留
    std::cout << "设备ID:" << this->info.deviceIp << std::endl;
    std::cout << "设备ip" << this->info.deviceIp << std::endl;
    std::cout << "设备端口" << this->info.devicePort << std::endl;
}

// 处理注册响应
int GB28181Connect::handleRegister(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    // 检查响应是否存在
    if (event->response != NULL)
    {
        // 获取响应状态码
        int status_code = event->response->status_code;
        printf("收到响应状态码: %d\n", status_code);
        // 如果是401 Unauthorized，需要添加认证信息并重试
        if (status_code == 401)
        {
            printf("注册失败，收到 401 Unauthorized 错误。\n");
            // 打印事件的事务ID
            printf("event->rid: %d\n", event->rid);
            // 定义WWW-Authenticate头指针
            osip_www_authenticate_t *www_authenticate_header = nullptr;
            int ret = 0;
            // 从响应消息中获取第一个WWW-Authenticate头
            ret = osip_message_get_www_authenticate(event->response, 0, &www_authenticate_header);
            // 检查获取是否成功，且头信息有效
            if (ret != 0 || www_authenticate_header == nullptr || www_authenticate_header->realm == NULL)
            {
                printf("获取返回信息失败\n");
                return -1;
            }
            // 添加认证信息：用户名、密码、认证域、算法（MD5）
            eXosip_lock(ctx); // 加锁保护eXosip内部状态
            /*
                16161616用户id需要与from头中的id相同
                密码是平台的密码
            */
            ret = eXosip_add_authentication_info(ctx, gbinfo.deviceName.c_str(), gbinfo.deviceName.c_str(), gbinfo.servePassword.c_str(), "MD5", www_authenticate_header->realm);
            eXosip_unlock(ctx);
            if (ret != 0)
            {
                printf("认证失败\n");
            }
            else
            {
                printf("认证成功\n");
            }
            // 注意：此处没有重新发送注册，通常应由上层循环处理
        }
        else
        {
            // 其他状态码视为注册失败
            printf("注册失败，状态码: %d\n", status_code);
        }
    }
    return 0;
    return 0; // 多余的return，可能是代码复制遗留
}

// 全局计数器，用于统计INVITE次数
int i = 0;

// 处理INVITE请求（来电），参数包括eXosip上下文、事件对象、设备信息和SDP信息结构体
int GB28181Connect::handleInvite(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo, SDPInfo &sdp_info)
{
    // 打印收到的INVITE序号
    printf("receive invite%d\n", i);
    // 如果存在响应则获取状态码（但INVITE请求一般不会有响应，这里可能是笔误）
    int status_code = event->response ? event->response->status_code : 0;
    printf("收到来电后的状态码：%d\n", status_code);
    // 获取SDP消息体
    osip_body_t *body = nullptr;
    // 检查请求是否存在
    if (event->request)
    {
        // 从请求中获取第一个消息体
        osip_message_get_body(event->request, 0, &body);
        // 如果消息体存在且非空
        if (body && body->body)
        {
            std::cout << "send body" << std::endl;
            std::cout << "body:" << body->body << std::endl;
            // 将消息体内容转为字符串
            std::string sdp_content(body->body);
            std::cout << "原始SDP内容:\n"
                      << sdp_content << std::endl;
            // 解析SDP内容
            parseSDP(sdp_content, sdp_info);
            // 构造200 OK响应消息
            osip_message_t *answer = nullptr;
            // 使用字符串流构建应答SDP
            std::stringstream ss;
            ss << "v=0\r\n"; // SDP版本
            // 源信息：用户名、会话ID（0）、版本（0）、网络类型、地址类型、设备IP
            ss << "o=" << sdp_info.origin.username << " 0 0 IN IP4 " << gbinfo.deviceIp << "\r\n";
            ss << "s=Play\r\n";                         // 会话名称
            ss << "u=" << gbinfo.deviceName << ":255\r\n"; // 非标准SDP字段，可能是GB28181扩展
            ss << "c=IN IP4 " << gbinfo.deviceIp << "\r\n"; // 连接信息
            ss << "t=0 0\r\n";                             // 会话时间
            ss << "m=video " << gbinfo.pushPort << " RTP/AVP 96\r\n"; // 媒体描述：视频，端口，协议，负载类型96
            ss << "a=rtpmap:96 PS/90000\r\n";                         // RTP映射，96对应PS流，时钟90000
            ss << "a=sendonly\r\n";                                    // 方向：只发送
            ss << "y=" << sdp_info.ssrc << "\r\n";                     // GB28181扩展，SSRC
            std::cout << "\n构造的SDP应答内容:\n"
                      << ss.str() << std::endl;
            std::string sdp_output_str = ss.str();

            osip_message_t *message = nullptr;

            printf("eXosip_call_build_answer\n");
            // 为这个呼叫构建200 OK应答
            int status = eXosip_call_build_answer(ctx, event->tid, 200, &message);
            if (status != 0)
            {
                std::cout << "eXosip_call_build_sdp_answer fail" << std::endl;
                return -1;
            }

            // 设置Content-Type为application/sdp
            osip_message_set_content_type(message, "APPLICATION/SDP");
            std::cout << "osip_message_set_content_type" << std::endl;
            // 设置消息体为构造的SDP字符串
            osip_message_set_body(message, sdp_output_str.c_str(), sdp_output_str.size());
            std::cout << "osip_message_set_body" << std::endl;
            {
                // 加锁保护eXosip调用
                std::lock_guard<std::mutex> lock(eXosip_mutex);
                // 发送200 OK应答
                int ret = eXosip_call_send_answer(ctx, event->tid, 200, message);
                if (ret != 0)
                {
                    printf("send sdp failed\n");
                }
            }

            std::cout << "eXosip_call_send_answer" << std::endl;
            return 0;
        }
    }
    return -1;
}

// 响应SUBSCRIBE请求中的移动位置查询（通过MESSAGE返回）
void GB28181Connect::respond_MobilePosition_by_subscribe(eXosip_t *ctx, int sn, const GB28Info &info)
{
    // 定义XML响应缓冲区
    char xml[1024];
    // 使用snprintf格式化XML字符串
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "<CmdType>MobilePosition</CmdType>\n"
             "<SN>%d</SN>\n"
             "<DeviceID>%s</DeviceID>\n"
             "<Result>OK</Result>\n"
             "</Response>",
             sn, info.deviceName.c_str());
    osip_message_t *notify = nullptr;
    // 构造目标地址（服务器）
    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    // 构造源地址（设备）
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    // 构建MESSAGE请求
    int ret = eXosip_message_build_request(ctx, &notify, "MESSAGE", to.c_str(), from.c_str(), NULL);
    if (ret != 0)
    {
        printf("build notify fail\n");
        return;
    }
    // 设置Content-Type为application/MANSCDP+xml，字符集GB2312
    osip_message_set_content_type(notify, "Application/MANSCDP+xml; charset=GB2312");
    // 设置消息体为XML内容
    osip_message_set_body(notify, xml, strlen(xml));

    // 发送MESSAGE请求
    if (eXosip_message_send_request(ctx, notify) < 0)
    {
        printf("Send MobilePosition MESSAGE failed.\n");
    }
    else
    {
        printf("MobilePosition MESSAGE response sent successfully.\n");
    }
}

// 处理SUBSCRIBE订阅请求
int GB28181Connect::handleSubcribe(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    printf("有订阅消息\n");

    // 检查请求是否存在
    if (event->request)
    {
        osip_body_t *body;
        // 获取请求消息体
        int ret = osip_message_get_body(event->request, 0, &body);
        printf("osip_message_get_body 返回值: %d\n", ret); // 打印返回值
        // 如果消息体获取成功且非空
        if (ret == 0 && body && body->body)
        {
            // 获取Content-Type头
            osip_content_type_t *content_type = osip_message_get_content_type(event->request);
            if (content_type && content_type->type && content_type->subtype)
            {
                // 提取类型和子类型
                std::string type(content_type->type);
                std::string subtype(content_type->subtype);
                // 转换为小写以便比较
                std::string type_lower = type;
                std::string subtype_lower = subtype;
                std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::transform(subtype_lower.begin(), subtype_lower.end(), subtype_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                // 如果是application/manscdp+xml类型
                if (type_lower == "application" && subtype_lower == "manscdp+xml")
                {
                    // 从消息体中提取CmdType
                    std::string cmdType = extractCmdType(body->body);
                    if (cmdType == "MobilePosition")
                    {
                        // 构建200 OK应答
                        osip_message_t *answer;
                        eXosip_insubscription_build_answer(ctx, event->tid, 200, &answer);
                        {
                            // 加锁发送应答
                            std::lock_guard<std::mutex> lock(eXosip_mutex);
                            eXosip_insubscription_send_answer(ctx, event->tid, 200, answer);
                            // 提取SN号
                            std::string sn = extractSN(body->body);
                            int num = std::stoi(sn);
                            // 通过MESSAGE响应移动位置
                            respond_MobilePosition_by_subscribe(ctx, num, info);
                        }
                    }
                    else
                    {
                        // 其他订阅（如目录订阅）
                        osip_message_t *answer;
                        eXosip_insubscription_build_answer(ctx, event->tid, 200, &answer);
                        {
                            std::lock_guard<std::mutex> lock(eXosip_mutex);
                            eXosip_insubscription_send_answer(ctx, event->tid, 200, answer);
                            printf("send successfully\n");
                            // 提取SN号
                            std::string sn = extractSN(body->body);
                            int num = std::stoi(sn);
                            // 发送目录列表（Catalog NOTIFY）
                            send_channel_list(ctx, num, gbinfo);
                        }
                    }
                }
            }
        }
        else
        {
            printf(" 没有消息体内容\n");
        }
    }
    return 0;
}

// 通过MESSAGE响应目录查询（Catalog）
void GB28181Connect::respond_catalog_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[2048];
    // 构造XML响应，包含设备列表（这里硬编码了一个设备）
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "  <CmdType>Catalog</CmdType>\n"
             "  <SN>%d</SN>\n"
             "  <DeviceID>%s</DeviceID>\n"
             "  <SumNum>1</SumNum>\n"
             "  <DeviceList>\n"
             "    <Item>\n"
             "      <DeviceID>%s</DeviceID>\n"
             "      <Name>test5</Name>\n"
             "      <Manufacturer>厂商名称</Manufacturer>\n"
             "      <Model>型号</Model>\n"
             "      <Owner>Owner</Owner>\n"
             "      <Parental>0</Parental>\n"
             "      <ParentID>%s</ParentID>\n"
             "      <SafetyWay>0</SafetyWay>\n"
             "      <RegisterWay>1</RegisterWay>\n"
             "      <Secrecy>0</Secrecy>\n"
             "      <Status>ON</Status>\n"
             "      <IPAddress>%s</IPAddress>\n"
             "      <Port>%d</Port>\n"
             "    </Item>\n"
             "  </DeviceList>\n"
             "</Response>",
             sn,
             info.deviceName.c_str(),
             info.deviceName.c_str(),
             info.deviceName.c_str(),
             info.serveIp.c_str(),
             atoi(info.servePort.c_str()));
    // 构造MESSAGE请求
    osip_message_t *msg = nullptr;

    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;

    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("Build MESSAGE failed.\n");
        return;
    }

    // 设置Content-Type
    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    // 设置消息体
    osip_message_set_body(msg, xml, strlen(xml));

    // 发送MESSAGE
    if (eXosip_message_send_request(ctx, msg) < 0)
    {
        printf("Send Catalog MESSAGE failed.\n");
    }
    else
    {
        printf("Catalog MESSAGE response sent successfully.\n");
    }
}

// 通过MESSAGE响应设备信息查询
void GB28181Connect::respond_deviceinfo_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[1024];
    // 构造XML响应，包含设备基本信息
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\" encoding=\"GB2312\"?>\n"
             "<Response>\n"
             "  <CmdType>DeviceInfo</CmdType>\n"
             "  <SN>%d</SN>\n"
             "  <DeviceID>%s</DeviceID>\n"
             "  <Result>OK</Result>\n"
             "  <DeviceName>GB28181-Device</DeviceName>\n"
             "  <Manufacturer>Manufacturer</Manufacturer>\n"
             "  <Model>Model</Model>\n"
             "  <Firmware>1.0</Firmware>\n"
             "</Response>",
             sn, info.deviceName.c_str());

    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    osip_message_t *msg = nullptr;
    // 构建MESSAGE请求
    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("DeviceInfo: Build MESSAGE failed.\n");
        return;
    }
    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(msg, xml, strlen(xml));
    // 发送MESSAGE
    if (eXosip_message_send_request(ctx, msg) < 0)
        printf("DeviceInfo: Send MESSAGE failed.\n");
    else
        printf("DeviceInfo response sent (SN=%d).\n", sn);
}

// 通过MESSAGE响应设备控制
void GB28181Connect::respond_deviceControl_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[1024];
    // 构造XML响应，表示控制结果OK
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "<CmdType>DeviceControl</CmdType>\n"
             "<SN>%d</SN>\n"
             "<DeviceID>%s</DeviceID>\n"
             "<Result>OK</Result>\n"
             "</Response>",
             sn, info.deviceName.c_str());
    osip_message_t *msg = nullptr;
    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    // 构建MESSAGE
    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("Build MESSAGE failed.\n");
        return;
    }
    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(msg, xml, strlen(xml));

    if (eXosip_message_send_request(ctx, msg) < 0)
    {
        printf("Send DeviceControl MESSAGE failed.\n");
    }
    else
    {
        printf("DeviceControl MESSAGE response sent successfully.\n");
    }
}

// 处理MESSAGE消息（即接收到的非订阅通知消息）
int GB28181Connect::handleNews(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    // 检查是否存在请求
    if (event->request)
    {
        // 事件类型23通常是MESSAGE（根据eXosip定义）
        if (event->type == 23)
        {
            osip_body_t *body;
            // 获取消息体
            int ret = osip_message_get_body(event->request, 0, &body);
            if (ret == 0 && body && body->body)
            {
                printf("Message body:\n%s\n", body->body);
                // 获取Content-Type
                osip_content_type_t *content_type = osip_message_get_content_type(event->request);
                if (content_type && content_type->type && content_type->subtype)
                {
                    std::string type(content_type->type);
                    std::string subtype(content_type->subtype);
                    // 转为小写比较
                    std::string type_lower = type;
                    std::string subtype_lower = subtype;
                    std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    std::transform(subtype_lower.begin(), subtype_lower.end(), subtype_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    // 如果是MANSCDP+xml类型
                    if (type_lower == "application" && subtype_lower == "manscdp+xml")
                    {
                        // 提取命令类型
                        std::string cmdType = extractCmdType(body->body);
                        if (cmdType == "DeviceInfo") // 设备信息查询
                        {
                            printf("DeviceInfo query received\n");
                            std::string sn_str = extractSN(body->body);
                            if (!sn_str.empty())
                            {
                                // 构建200 OK应答
                                osip_message_t *answer = nullptr;
                                eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                                if (answer)
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(eXosip_mutex);
                                        eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    }
                                    int sn_num = std::stoi(sn_str);
                                    // 通过MESSAGE返回设备信息
                                    respond_deviceinfo_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                        else if (cmdType == "Catalog") // 目录查询
                        {
                            printf("catalog query message\n");
                            std::string sn = extractSN(body->body);
                            osip_message_t *answer = nullptr;
                            eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                            if (answer)
                            {
                                {
                                    std::lock_guard<std::mutex> lock(eXosip_mutex);
                                    eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    int sn_num = std::stoi(sn);
                                    // 通过MESSAGE返回目录
                                    respond_catalog_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                        else if (cmdType == "DeviceControl") // 设备控制
                        {
                            printf("DeviceControl query message\n");
                            std::string sn = extractSN(body->body);
                            osip_message_t *answer = nullptr;
                            eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                            if (answer)
                            {
                                {
                                    std::lock_guard<std::mutex> lock(eXosip_mutex);
                                    eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    int sn_num = std::stoi(sn);
                                    // 通过MESSAGE返回控制结果
                                    respond_deviceControl_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                    }

                    // 以下是之前的代码片段，可能冗余
                    // 1. 提取SN（如果是订阅消息）
                    std::string sn = extractSN(body->body);
                    if (!sn.empty())
                    {
                        printf("Extracted SN: %s\n", sn.c_str());

                        // 2. 构建并发送200 OK响应（使用message的API，不是call的API）
                    }
                }
                else
                {
                    printf("No message body or parse failed\n");
                }
            }
        }
    }
    return 0;
}

// 发送注册请求
int GB28181Connect::send_register(eXosip_t *ctx, GB28Info info)
{
    osip_message_t *reg = NULL;
    char *contact = NULL;          // contact头，这里设置为NULL，eXosip会自动生成
    int registration_id;

    // 构建 To 和 From 头
    string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    printf("from:%s\n", from.c_str());
    // 创建初始REGISTER请求，过期时间60秒
    registration_id = eXosip_register_build_initial_register(ctx, from.c_str(), to.c_str(), contact, 60, &reg);
    if (registration_id < 0)
    {
        std::cerr << "[SIP] 构建 REGISTER 请求失败！\n";
        return -1;
    }
    printf("registration_id:%d\n", registration_id);

    // 发送REGISTER请求
    {
        std::lock_guard<std::mutex> lock(eXosip_mutex);
        if (eXosip_register_send_register(ctx, registration_id, reg) != 0)
        {
            std::cerr << "[SIP] 发送 REGISTER 请求失败！\n";
            return -1;
        }
    }

    std::cout << "[SIP] REGISTER 请求已发送。\n";
    return registration_id; // 返回注册ID，用于后续刷新
}

// 发送目录列表NOTIFY（用于订阅响应）
void GB28181Connect::send_channel_list(eXosip_t *ctx, int sn, GB28Info info)
{
    char tmp[2048]; // 临时缓冲区
    // 定义XML模板（注意：这里模板中的格式化占位符可能错误，但原代码如此）
    const char *xml =
        "<?xml version=\"1.0\"?>\n"
        "<Response>\n"
        "  <CmdType>Catalog</CmdType>\n"
        "  <SN>%d</SN>\n"
        "  <DeviceID>%lld</DeviceID>\n"
        "  <SumNum>1</SumNum>\n"
        "  <DeviceList>\n"
        "    <Item>\n"
        "      <DeviceID>%lld</DeviceID>\n"
        "      <Name>通道1</Name>\n"
        "      <Manufacturer>厂商名称</Manufacturer>\n"
        "      <Model>型号</Model>\n"
        "      <Owner>Owner</Owner>\n"
        "      <Parental>0</Parental>\n"
        "      <ParentID>%lld</ParentID>\n"
        "      <SafetyWay>0</SafetyWay>\n"
        "      <RegisterWay>1</RegisterWay>\n"
        "      <Secrecy>0</Secrecy>\n"
        "      <Status>ON</Status>\n"
        "      <IPAddress>%s</IPAddress>\n"
        "      <Port>%d</Port>\n"
        "    </Item>\n"
        "  </DeviceList>\n"
        "</Response>";
    char formatted_xml[2048];
    // 格式化XML，将SN、设备ID等填入（注意使用了atoll和atoi，可能有问题）
    snprintf(formatted_xml, sizeof(formatted_xml), xml, sn, atoll(info.deviceName.c_str()), atoll(info.deviceName.c_str()), atoll(info.deviceName.c_str()), info.serveIp.c_str(), atoi(info.serveIp.c_str()));

    osip_message_t *notify = NULL;
    // 构造目标地址和源地址
    string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    const char *eventvalue = "Catalog"; // 事件类型
    const char *msgtype = "Application/MANSCDP+xml; charset=GB2312";
    // 构建NOTIFY请求
    int ret = eXosip_message_build_request(ctx, &notify, "NOTIFY", to.c_str(), from.c_str(), NULL);
    if (ret != 0)
    {
        printf("build notify fail\n");
        return;
    }
    // 设置Event头
    osip_message_set_header(notify, "Event", eventvalue);
    // 设置Subscription-State为active
    osip_message_set_header(notify, "Subscription-State", "active");
    if (notify != NULL)
    {
        // 复制格式化后的XML到临时缓冲区
        strncpy(tmp, formatted_xml, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        // 设置消息体
        osip_message_set_body(notify, tmp, strlen(tmp));
        // 设置Content-Type
        osip_message_set_content_type(notify, msgtype);
    }

    // 发送NOTIFY
    ret = eXosip_message_send_request(ctx, notify);
    if (ret < 0)
    {
        printf("send notify fail\n");
    }
    else
    {
        printf("Catalog NOTIFY sent successfully\n");
    }
}

// 注册刷新线程函数，每隔一定时间重新发送注册以保持注册状态
void register_thread(int second, eXosip_t *ctx, eXosip_event_t *event, int rID)
{
    // 无限循环
    while (1)
    {
        // 等待指定秒数
        std::this_thread::sleep_for(std::chrono::seconds(second));
        int ret = 0;
        osip_message_t *register_message = NULL;
        eXosip_lock(ctx); // 加锁
        // 构建刷新注册请求
        ret = eXosip_register_build_register(ctx, rID, 60, &register_message);
        eXosip_unlock(ctx);
        if (ret != 0)
        {
            printf("eXosip_register_build_register fail:%d\n", ret);
            // 等待5秒后继续重试
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        else
        {
            printf("eXosip_register_build_register success:%d\n", ret);
        }
        // 发送注册刷新请求
        {
            std::lock_guard<std::mutex> lock(eXosip_mutex);
            ret = eXosip_register_send_register(ctx, event->rid, register_message);
            if (ret != 0)
            {
                printf("eXosip_register_send_register fail%d\n", ret);
            }
            else
            {
                printf("re REGISTER message sent successfully\n");
            }
        }
    }
}