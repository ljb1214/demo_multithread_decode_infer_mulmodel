/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

// 包含 PS/RTP 封装头文件
#include "ps_rtp.h"
// 包含 GB28181 信令处理头文件
#include "gb28181.h"
// 包含视频解码/编码头文件（YuvtoH264 等）
#include "decode.h"
// 包含推流主控头文件
#include "push.h"
// 包含配置读取头文件
#include "config.h"
// 原子操作库，用于线程安全的标志位
#include <atomic>
// 条件变量，用于线程同步
#include <condition_variable>
// 互斥锁
#include <mutex>
// 线程库
#include <thread>

// 全局标志：工作线程是否应停止
std::atomic<bool> work_shut(false);
// 全局标志：是否需要检查队列
std::atomic<bool> is_check(true);
// 全局标志：第一帧标志（用于特殊处理第一帧）
std::atomic<bool> first_frame_flg(true);
// 全局标志：GB28181 整体是否停止
std::atomic<bool> gb_stop(false);

// 互斥锁和条件变量，用于控制队列检查
std::mutex is_check_mtx;
std::condition_variable cond_check;

// SIP 线程对象
static std::thread g_sip_thread;
// 编码后的 H.264 数据队列，容量为 50
FrameQueue<H264Frame> h264data(50);

/**
 * 工作线程函数：从 h264data 队列中取出编码数据，封装为 PS/RTP 并发送
 * @param sdp_info SDP 信息（包含目标 IP 和端口）
 * @param udp_socket UDP 套接字
 * @param packer RTP 打包上下文（序列号、时间戳等）
 */
void work(SDPInfo &sdp_info, int udp_socket, Data_Info_s &packer)
{
    int ul = 1;
    // 将 UDP 套接字设置为非阻塞模式
    int ret = ioctl(udp_socket, FIONBIO, &ul);
    bool is_first = true;      // 标记是否为第一帧（特殊处理）
    if (ret == -1)
    {
        printf("设置非阻塞失败!");
        return;
    }
    std::cout << "sdp_info.ssrc" << sdp_info.ssrc << std::endl;
    printf("u32Ssrc=%d\n", packer.u32Ssrc);
    H264Frame data;                     // 存储从队列中取出的帧
    int count = 0;
    // 分配 1MB 缓冲区用于暂存数据（实际用于零拷贝打包）
    std::unique_ptr<char[]> bufdata(new char[1024 * 1024]);
    char *buf = bufdata.get();
    printf("h264data work:%ld\n", h264data.size());
    while (true)
    {
        // 如果工作线程被要求停止，则退出循环
        if (work_shut)
        {
            printf("work_shut\n");
            break;
        }
        // 阻塞等待队列中有数据可取
        if (!h264data.wait_and_pop(data))
        {
            printf("have no data\n");
            continue;
        }

        // 第一帧特殊处理：需要发送 SPS/PPS/IDR 等
        if (is_first)
        {
            printf("first frame1\n");
            if (first_frame_flg == true)
            {
                printf("first_frame_flg is true\n");
            }
            else
            {
                printf("first_frame_flg is false\n");
            }
            // 将 H.264 数据拆分为 NAL 单元（SPS, PPS, IDR 等）
            auto nalus = splitNalus(reinterpret_cast<const char *>(data.data.data()),
                                    static_cast<int>(data.data.size()));
            printf("first frame size:%ld\n", nalus.size());
            // 调用专用函数发送第一帧（包含 PS 头 + 系统头 + PSM + SPS/PPS/IDR 分片）
            int ret = gb28181_streampackageForH264_first(nalus, &packer, 0,
                                                         sdp_info.connectionAddress.ip.c_str(),
                                                         static_cast<uint16_t>(std::stoul(sdp_info.mediaStreams.port)),
                                                         udp_socket);
            if (ret < 0)
            {
                printf("first send fail:%d\n", ret);
                return;
            }
            // 增加时间戳（模拟 33.33ms 一帧，即 30fps，90000/30 = 3000）
            packer.s64CurPts += 3000;
            // 睡眠 33.33ms 以控制帧率
            usleep(1000000 / 30);
            is_first = false;
        }
        else
        {
            // 非第一帧：直接在数据前面预留 PES 头空间（零拷贝）
            memcpy(buf + PES_HDR_LEN, data.data.data(), data.data.size());
            // 调用普通封装发送函数（不含系统头和 PSM）
            gb28181_streampackageForH264(buf + PES_HDR_LEN,
                                         static_cast<int>(data.data.size()),
                                         &packer,
                                         0,   // stream_type: 0=视频
                                         sdp_info.connectionAddress.ip.c_str(),
                                         static_cast<uint16_t>(std::stoul(sdp_info.mediaStreams.port)),
                                         udp_socket);
            packer.s64CurPts += 3000;  // 时间戳递增
            usleep(1000000 / 30);      // 30fps 帧率控制
        }
    }
    // 循环结束，关闭套接字
    close(udp_socket);
    return;
}

// 存储解码后的图像帧（Mat 格式），队列容量 10
FrameQueue<cv::Mat> matData(10);

/**
 * 将 BGR 格式的 Mat 图像存入队列（用于编码）
 * @param img 输入的 BGR 图像
 * @return 0 成功，-1 失败
 */
int set_Mat(cv::Mat &img)
{
    if (img.empty())
        return -1;
    // 深拷贝 Mat，避免局部变量销毁后数据指针失效
    cv::Mat img_copy = img.clone();
    if (!matData.push(img_copy))
    {
        printf("set mat failed\n");
        return -1;
    }
    return 0;
}

/**
 * 将 YUV420 数据转换为 BGR 并存入队列
 * @param y_data Y 分量数据
 * @param u_data U 分量数据
 * @param v_data V 分量数据
 * @param width 图像宽度
 * @param height 图像高度
 * @return 0 成功，-1 失败
 */
int set_Frame(uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, int width, int height)
{
    if (!y_data || !u_data || !v_data || width <= 0 || height <= 0)
        return -1;

    // 创建 YUV420 格式的 Mat（单通道，大小 = height * 3/2 行，width 列）
    cv::Mat yuv_mat(height * 3 / 2, width, CV_8UC1);

    // 拷贝 Y 分量
    memcpy(yuv_mat.data, y_data, width * height);
    // 拷贝 U 分量
    memcpy(yuv_mat.data + width * height, u_data, width * height / 4);
    // 拷贝 V 分量
    memcpy(yuv_mat.data + width * height * 5 / 4, v_data, width * height / 4);

    // 将 YUV420 转换为 BGR 格式
    cv::Mat bgr_mat;
    cv::cvtColor(yuv_mat, bgr_mat, cv::COLOR_YUV2BGR_I420);

    // 存入队列
    return set_Mat(bgr_mat);
}

/**
 * 通知解码线程结束（用于关闭队列）
 */
void notify_decode_finished()
{
    matData.shutdown(); // 关闭队列，让等待线程退出
}

/**
 * 编码线程函数：从 matData 队列中获取 BGR 图像，缩放并编码为 H.264，然后存入 h264data 队列
 */
void encode()
{
    Mat data;            // 临时存储 BGR 图像
    Mat frame_yuv;       // 临时存储 YUV 图像
    Mat resized_data;    // 缩放后的图像
    while (matData.wait_and_pop(data)) // 阻塞等待图像帧
    {
        if (work_shut)
        {
            break; // 工作停止，退出
        }
        // 检查图像有效性
        if (data.empty() || data.rows <= 0 || data.cols <= 0)
        {
            printf("is empty\n");
            continue;
        }
        try
        {
            // 将图像缩放到 1280x720（编码器要求的分辨率）
            cv::resize(data, resized_data, cv::Size(1280, 720));

            // 如果 YUV 缓冲区未分配或尺寸不匹配，则创建
            if (frame_yuv.empty() || frame_yuv.cols != resized_data.cols || frame_yuv.rows != resized_data.rows * 3 / 2)
            {
                frame_yuv.create(resized_data.rows * 3 / 2, resized_data.cols, CV_8UC1);
            }

            // 将 BGR 转换为 YUV420 I420 格式
            cv::cvtColor(resized_data, frame_yuv, cv::COLOR_BGR2YUV_I420);
        }
        catch (const cv::Exception &e)
        {
            printf("OpenCV error, skip frame: %s\n", e.what());
            continue;
        }

        // 准备编码缓冲区（足够大）
        const size_t buf_size = frame_yuv.total() * frame_yuv.elemSize();
        std::vector<std::uint8_t> buf(buf_size);
        char *encode_buf = reinterpret_cast<char *>(buf.data());
        size_t encode_len = buf_size;

        // 调用硬件编码函数（YUV 转 H.264）
        YuvtoH264(resized_data.cols, resized_data.rows, frame_yuv, encode_buf, encode_len);
        buf.resize(encode_len); // 调整实际编码后的大小

        // 构造 H264Frame 对象并放入队列
        H264Frame h264_frame;
        h264_frame.data = std::move(buf);
        h264data.push(h264_frame);
    }
    // 编码结束，关闭输出队列
    h264data.shutdown();
    printf("encode exit\n");
    return;
}

/**
 * 队列检查线程：定期检查 matData 队列是否已满，若满则清空（避免内存积压）
 */
void checkQueueThread()
{
    while (!gb_stop.load())
    {
        // 等待条件变量通知，或 is_check 为 true 时检查
        std::unique_lock<std::mutex> lock(is_check_mtx);
        cond_check.wait(lock, []()
                        { return is_check.load(); });
        // 如果队列大小达到 10，则清空
        if (matData.size() == 10)
        {
            std::cout << "need to clear queue" << std::endl;
            matData.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 30ms 检查一次
    }
}

/**
 * SIP 信令线程：处理 GB28181 信令交互，启动编码和推流线程
 * @param gbinvite GB28181 连接对象
 * @param sdp_info SDP 信息（会从 INVITE 中解析填充）
 * @param gbinfo 设备配置信息
 */
void SipThread(GB28181Connect gbinvite, SDPInfo sdp_info, GB28Info gbinfo)
{
    int registerID;
    std::atomic<bool> first_register_thread(true); // 是否已启动注册刷新线程
    std::thread encode_thread;                     // 编码线程句柄
    std::thread work_thread;                       // 推流工作线程句柄
    std::thread check_thread(checkQueueThread);    // 队列检查线程
    Data_Info_s packer;                            // RTP 打包上下文
    int udp_socket = -1;                           // UDP 套接字
    int local_push_port = 0;                       // 本地推流端口（从配置获取）
    eXosip_t *ctx;                                 // eXosip 句柄
    int ret = 0;
    try
    {
        local_push_port = std::stoi(gbinfo.pushPort);
    }
    catch (const std::exception &)
    {
        fprintf(stderr, "invalid pushPort: %s\n", gbinfo.pushPort.c_str());
        return;
    }
    // 初始化 eXosip
    ctx = eXosip_malloc();
    if (ctx == NULL)
    {
        fprintf(stderr, "Could not initialize eXosip2.\n");
        return;
    }

    if (eXosip_init(ctx) != 0)
    {
        std::cerr << "[SIP] 初始化 eXosip 失败！\n";
        return;
    }
    // 设置本地监听地址和端口（设备端口）
    ret = eXosip_listen_addr(ctx, IPPROTO_UDP, NULL, atoi(gbinfo.devicePort.c_str()), AF_INET, 0);
    if (ret != 0)
    {
        fprintf(stderr, "Could not set listening address.\n");
        eXosip_quit(ctx);
        return;
    }

    printf("eXosip2 初始化成功\n");

    // 发送初始 REGISTER 请求
    registerID = gbinvite.send_register(ctx, gbinfo);

    // 事件循环
    while (!gb_stop.load())
    {
        // 等待 SIP 事件（超时 40ms）
        eXosip_event_t *event;
        event = eXosip_event_wait(ctx, 0, 40);
        eXosip_lock(ctx);
        eXosip_automatic_action(ctx); // 执行自动操作（如重传）
        eXosip_unlock(ctx);
        if (event == NULL)
        {
            continue;
        }
        printf("receive news: %d\n", event->type);

        // 根据事件类型分发处理
        switch (event->type)
        {
        case EXOSIP_REGISTRATION_SUCCESS:
        {
            printf("返回码:%d\n", event->response->status_code);
            printf("register success。\n");
            // 首次注册成功时，启动注册刷新线程（每 30 秒刷新一次）
            if (first_register_thread.load())
            {
                std::thread t(register_thread, 30, ctx, event, registerID);
                t.detach(); // 线程分离，独立运行
                first_register_thread.store(false);
            }
        }
        break;
        case EXOSIP_REGISTRATION_FAILURE:
        {
            // 处理注册失败（如 401 认证）
            gbinvite.handleRegister(ctx, event, gbinfo);
        }
        break;

        case EXOSIP_MESSAGE_NEW:
        {
            printf("Received SIP message\n");
            // 处理 MESSAGE 请求（如设备信息查询、目录查询等）
            gbinvite.handleNews(ctx, event, gbinfo);
            break;
        }
        case EXOSIP_CALL_ACK:
        {
            printf("start ask\n");
            // 收到 ACK 应答，表示媒体会话已建立，开始推流
            is_check.store(false);          // 停止队列检查（推流过程中不需要检查）
            h264data.reset(50);             // 重置编码数据队列
            printf("start com\n");
            // 创建 UDP 套接字用于推流
            udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_socket < 0)
            {
                perror("socket creation failed");
                return;
            }
            // 绑定本地地址和端口（设备 IP + 推流端口）
            struct sockaddr_in addr_local;
            memset(&addr_local, 0, sizeof(addr_local));
            addr_local.sin_family = AF_INET;
            addr_local.sin_port = htons(local_push_port);
            addr_local.sin_addr.s_addr = inet_addr(gbinfo.deviceIp.c_str());
            if (udp_socket < 0)
            {
                printf("socket has been close\n");
            }
            if (bind(udp_socket, (struct sockaddr *)&addr_local, sizeof(addr_local)) < 0)
            {
                perror("bind failed");
                printf("bind local media addr failed, ip=%s port=%d\n", gbinfo.deviceIp.c_str(), local_push_port);
                close(udp_socket);
                return;
            }
            // 重置第一帧标志
            first_frame_flg.store(true);
            // 初始化 RTP 打包上下文
            packer.s64CurPts = 0;
            packer.u32Ssrc = atoi(sdp_info.ssrc.c_str());
            packer.u16CSeq = 0;
            printf("receive answer\n");
            work_shut = false;
            // 启动编码线程
            encode_thread = std::thread(encode);
            printf("start work thred\n");
            // 启动推流工作线程
            work_thread = std::thread(work, std::ref(sdp_info), udp_socket, std::ref(packer));
            break;
        }
        case EXOSIP_IN_SUBSCRIPTION_NEW:
        {
            // 处理订阅请求（如移动位置、目录订阅）
            gbinvite.handleSubcribe(ctx, event, gbinfo);
            break;
        }
        case EXOSIP_CALL_CLOSED:
        {
            printf("close\n");
            // 会话关闭，停止推流
            is_check.store(true);               // 重新启用队列检查
            work_shut = true;                   // 通知工作线程停止
            cond_check.notify_one();            // 唤醒检查线程（使其可以清理队列）
            if (udp_socket >= 0)
            {
                close(udp_socket);
                udp_socket = -1;
            }
            // 等待编码线程结束
            if (encode_thread.joinable())
            {
                encode_thread.join();
                printf("encode_thread stopped\n");
            }
            // 等待推流工作线程结束
            if (work_thread.joinable())
            {
                work_thread.join();
                printf("work_thread stopped\n");
            }
            break;
        }
        case EXOSIP_CALL_INVITE:
            // 处理 INVITE 请求（接收平台发起的呼叫）
            gbinvite.handleInvite(ctx, event, gbinfo, sdp_info);
            break;
        }
        eXosip_event_free(event); // 释放事件内存
    }
    // 退出主循环，清理资源
    work_shut = true;
    cond_check.notify_all();               // 唤醒所有等待的线程
    if (encode_thread.joinable())
    {
        encode_thread.join();
    }
    if (work_thread.joinable())
    {
        work_thread.join();
    }
    gb_stop.store(true);
    cond_check.notify_all();
    if (check_thread.joinable())
    {
        check_thread.join();
    }
    // 退出 eXosip
    eXosip_quit(ctx);
    free(ctx);
}

/**
 * 启动 GB28181 推流（外部调用入口）
 * @return 0 成功，-1 失败
 */
int pushToGB28181()
{
    // 获取全局配置
    const GBConfig &cfg = get_gb_config();

    GB28Info gbinfo;
    gbinfo.devicePort = cfg.devicePort;
    gbinfo.deviceIp = cfg.deviceIp;
    gbinfo.deviceName = cfg.deviceName;
    gbinfo.servePort = cfg.servePort;
    gbinfo.serveIp = cfg.serveIp;
    gbinfo.serveName = cfg.serveName;
    gbinfo.servePassword = cfg.servePassword;
    gbinfo.pushPort = cfg.pushPort;

    SDPInfo sdp_info;               // 临时 SDP 信息（后续从 INVITE 中填充）
    GB28181Connect gbinvite(gbinfo); // 创建连接对象

    gb_stop.store(false);
    work_shut.store(false);
    is_check.store(true);

    try
    {
        printf("aaa\n");
        // 启动 SIP 信令线程
        g_sip_thread = std::thread(SipThread, gbinvite, sdp_info, gbinfo);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception caught while creating SIP thread: " << e.what() << std::endl;
        return -1;
    }
}

/**
 * 停止 GB28181 推流（外部调用入口）
 */
void stopGB28181()
{
    gb_stop.store(true);      // 通知停止
    work_shut.store(true);
    cond_check.notify_all();  // 唤醒所有等待线程
    if (g_sip_thread.joinable())
    {
        g_sip_thread.join();  // 等待 SIP 线程结束
    }
}