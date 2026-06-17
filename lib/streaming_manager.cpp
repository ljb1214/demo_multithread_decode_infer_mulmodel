
// 包含流媒体管理类的头文件（类定义、结构体、接口声明）
#include "streaming_manager.h"
// 标准输入输出，用于打印日志、错误信息
#include <iostream>
// 文件操作库（本代码未直接使用，属于工程通用包含）
#include <fstream>
// 字符串流，用于拼接字符串（如生成JSON、时间字符串）
#include <sstream>
// 输入输出格式化，用于时间格式化输出
#include <iomanip>
// C风格字符串操作函数，如memcpy、memset
#include <cstring>
// C++ 时间库，用于时间戳、帧率计算、延时控制
#include <chrono>
// 动态数组，用于存储编码后的H264码流数据
#include <vector>

// FFmpeg是C语言库，C++调用必须加extern "C"防止名字修饰错误
extern "C" {
// FFmpeg 封装格式处理核心（处理MP4、FLV、RTMP、RTSP等）
#include <libavformat/avformat.h>
// FFmpeg 编解码参数、格式定义
#include <libavcodec/avcodec.h>
// FFmpeg 基础工具库
#include <libavutil/avutil.h>
// FFmpeg 图像处理工具库
#include <libavutil/imgutils.h>
}

// 匿名命名空间：内部辅助函数，外部无法访问，避免命名冲突
namespace {

// 功能：填充FFmpeg视频流参数（编码格式、宽高、像素格式）
// 参数：stream - FFmpeg流；width/height - 视频宽高
bool fillCodecParameters(AVStream* stream, int width, int height)
{
    // 检查流和参数指针是否有效
    if (!stream || !stream->codecpar) {
        return false;
    }

    // 获取流的编码参数结构体
    AVCodecParameters* codecpar = stream->codecpar;
    // 媒体类型：视频
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    // 编码格式：H.264
    codecpar->codec_id = AV_CODEC_ID_H264;
    // 设置视频宽度
    codecpar->width = width;
    // 设置视频高度
    codecpar->height = height;
    // 像素格式：YUV420P（RK MPP硬件编码器标准输出格式）
    codecpar->format = AV_PIX_FMT_YUV420P;
    // 时间基：时间单位，初始化为1/1，后续会重新设置
    stream->time_base = AVRational{1, 1};
    return true;
}

// 功能：从RK MPP编码器获取SPS/PPS信息，写入FFmpeg的extradata
// 作用：让播放器正确识别H264流，必须设置，否则无法解码
bool attachEncoderExtradata(AVCodecParameters* codecpar, MppEncoder* encoder)
{
    // 检查参数是否有效
    if (!codecpar || !encoder) {
        return false;
    }

    // 临时缓冲区，存储SPS/PPS数据
    uint8_t header_buf[1024];
    int header_size = sizeof(header_buf);
    // 从MPP编码器获取H264的SPS/PPS头信息
    if (encoder->GetHeader(header_buf, &header_size) != 0 || header_size <= 0) {
        std::cerr << "Warning: failed to get H.264 extra info from MPP encoder" << std::endl;
        return false;
    }

    // 为FFmpeg分配extradata内存，必须额外加64字节填充区防止越界
    codecpar->extradata = static_cast<uint8_t*>(av_malloc(header_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codecpar->extradata) {
        std::cerr << "Failed to allocate extradata buffer" << std::endl;
        return false;
    }

    // 复制SPS/PPS数据到FFmpeg参数
    memcpy(codecpar->extradata, header_buf, header_size);
    // 填充区清零
    memset(codecpar->extradata + header_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    // 设置extradata长度
    codecpar->extradata_size = header_size;
    std::cout << "H.264 extradata from MPP, size: " << header_size << " bytes" << std::endl;
    return true;
}
} // namespace

// ==================== StreamingManager 类成员函数实现 ====================

// 构造函数：初始化所有成员变量为默认安全值
StreamingManager::StreamingManager()
    : streaming_active_(false)        // 推流状态：未启动
    , should_stop_(false)             // 停止标志：未停止
    , rtmp_context_(nullptr)          // RTMP上下文为空
    , rtsp_context_(nullptr)          // RTSP上下文为空
    , mpp_encoder_(nullptr)           // 硬件编码器对象为空
    , frame_index_(0)                 // 帧序号从0开始
    , frame_count_(0)                 // 帧率统计计数器清零
{
}

// 析构函数：自动调用停止函数，确保资源全部释放，不泄漏
StreamingManager::~StreamingManager() {
    stopStreaming();
}

// 功能：初始化推流管理器（配置参数、编码器、网络）
bool StreamingManager::initialize(const StreamingConfig& config) {
    // 保存外部传入的配置
    config_ = config;    
    // 初始化统计时间戳和计数帧率，为后续计算帧率做准备                          
    stats_.last_frame_time = std::chrono::system_clock::now();  
    // 帧序号重置
    frame_index_ = 0;  
    // 帧率计数重置                            
    frame_count_ = 0;                             

    // 判断是否需要启用网络推流
    const bool need_network_output = config_.enable_rtmp || config_.enable_rtsp;
    // 不需要推流直接返回成功
    if (!need_network_output) {                    
        return true;
    }
    // 初始化FFmpeg网络库（RTMP/RTSP依赖）
    avformat_network_init();                       
    // 初始化RK MPP硬件编码器
    if (!ensureEncoderInitialized()) {             
        return false;
    }
    // 如果开启RTMP
    if (config_.enable_rtmp) { 
        // 初始化RTMP推流                    
        if (!initializeRTMP()) {
            // 打印初始化失败信息                   
            std::cerr << "Failed to initialize RTMP streaming" << std::endl;
            // 初始化失败清理RTSP
            closeOutput(rtsp_context_, "RTSP"); 
            // 返回失败   
            return false;
        }
    }
    // 如果开启RTSP
    if (config_.enable_rtsp) {  
        // 初始化RTSP推流                   
        if (!initializeRTSP()) {   
            // 打印初始化失败信息                
            std::cerr << "Failed to initialize RTSP streaming" << std::endl;
            // 失败清理RTMP
            closeOutput(rtmp_context_, "RTMP");    
            return false;
        }
    }
    // 初始化成功
    return true;
}

// 功能：向推流队列添加一帧图像（外部调用）
void StreamingManager::addStreamingData(const StreamingData& data) {
    // 线程锁，保证队列安全
    std::lock_guard<std::mutex> lock(queue_mutex_);  
    // 队列缓冲上限10帧，防止堆积
    if (streaming_queue_.size() > 10) { 
        // 丢弃最旧帧             
        streaming_queue_.pop(); 
        // 丢帧计数+1                     
        stats_.frames_dropped++;                     
    }
    // 将帧数据推入队列
    streaming_queue_.push(data); 
    // 唤醒推流线程处理数据                    
    queue_cv_.notify_one();                          

    // 调试日志：每100帧打印一次队列长度
    static int frame_count = 0;
    // 计数到 100帧打印一次当前队列大小，监控推流状态和性能
    if (++frame_count % 100 == 0) {
        std::cout << "Added frame to streaming queue, queue size: " << streaming_queue_.size() << std::endl;
    }
}

// 功能：启动推流线程
void StreamingManager::startStreaming() {
    // 已启动则直接返回
    if (streaming_active_.load()) {                  
        return;
    }
    // 设置推流启动标志
    streaming_active_ = true;  
    // 清除停止标志                      
    should_stop_ = false;                            
    // 创建工作线程，执行streamingWorker
    streaming_thread_ = std::thread(&StreamingManager::streamingWorker, this);
    // 打印推流开始信息
    std::cout << "Streaming started" << std::endl;
}

// 功能：停止推流，释放所有资源（线程安全、优雅退出）
void StreamingManager::stopStreaming() {
    // 未启动则直接清理资源
    if (!streaming_active_.load()) { 
        // 关闭输出，释放RTSP/RTMP资源                
        closeOutput(rtmp_context_, "RTMP");
        closeOutput(rtsp_context_, "RTSP");
        // 释放 MPP 编码器资源
        if (mpp_encoder_) {
            // 释放资源 api 函数，跟据 MPP 释放资源的规范，先调用Relese， 在delete对象，最后指针置空，防止野指针
            mpp_encoder_->Release();
            delete mpp_encoder_;
            // 编码器指针置空，防止野指针
            mpp_encoder_ = nullptr;
        }
        return;
    }
    // 通知线程退出
    should_stop_ = true;  
    // 唤醒所有等待的线程                           
    queue_cv_.notify_all();                          
    // 等待工作线程结束
    if (streaming_thread_.joinable()) {              
        streaming_thread_.join();
    }
    // 清除运行标志
    streaming_active_ = false;                       
    // 关闭RTMP
    closeOutput(rtmp_context_, "RTMP");   
    // 关闭RTSP           
    closeOutput(rtsp_context_, "RTSP");              
     // 释放MPP硬件编码器资源
    if (mpp_encoder_) { 
        // 释放资源 api 函数，跟据 MPP 释放资源的规范，先调用Relese， 在delete对象，最后指针置空，防止野指针                            
        mpp_encoder_->Release();
        // 指针置空，防止野指针
        delete mpp_encoder_;
        mpp_encoder_ = nullptr;
    }
    // 打印推流停止信息
    std::cout << "Streaming stopped" << std::endl;
}

// 功能：推流工作线程（核心循环：取帧→绘制→编码→发送→控帧）
void StreamingManager::streamingWorker() {
     // 缓存最后一帧，队列为空时重复发送
    cv::Mat last_frame;   
    // 是否有有效帧           
    bool has_last_frame = false;      
    // 没有收到停止命令则一直循环
    while (!should_stop_.load()) {    
        // 从队列取出一帧数据，等待超时控制帧率，队列流为空发送最后一帧保持流不断开
        StreamingData data;
        bool got_data = false;

        {
            // 手动加锁保护队列安全
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // 等待超时时间：1000/fps 毫秒，控制帧率
            auto timeout = std::chrono::milliseconds(1000 / config_.fps);
            // 等待队列非空 或 停止信号
            if (queue_cv_.wait_for(lock, timeout, [this] {
                // 等待条件，队列非空或受到停止信号，返回return。继续执行循环体
                return !streaming_queue_.empty() || should_stop_.load();
            })) {
                // 收到停止信号，退出循环
                if (should_stop_.load()) {  
                    // 退出循环   
                    break;
                }
                // 队列如果非空，取出一帧数据处理
                if (!streaming_queue_.empty()) { 
                    // 取出队列数据  
                    data = streaming_queue_.front();
                    // 从队列中弹出已取出的数据，继续等待下一帧的数据到来
                    streaming_queue_.pop();
                    // 标记已经成功获取到数据，可以进行后续处理（绘制、编码、发送等）
                    got_data = true;
                }
            } else {   // 等待超时：队列为空，发送上一帧保持流不断开
                if (has_last_frame && !last_frame.empty()) {
                    // 复制生成新的数据对象，保持数据不变，保存流不断，防止播放器断流
                    data.frame = last_frame.clone();
                    // 流id和时间戳可以保持不变，表示反复发送最后一帧
                    data.stream_id = 0;
                    // 时间戳跟新为当前时间，内容一样，时间不同
                    data.timestamp = std::chrono::system_clock::now();
                    // 清空检测结果（复用旧帧）
                    memset(&data.person_results, 0, sizeof(detect_result_group_t));
                    memset(&data.helmet_results, 0, sizeof(detect_result_group_t));
                    memset(&data.tired_results, 0, sizeof(detect_result_group_t));
                    memset(&data.callplay_results, 0, sizeof(detect_result_group_t));
                    // 标记已经收到数据，虽然是重复帧
                    got_data = true;
                } else {
                    // 无缓存帧，继续等待
                    continue;    
                }
            }
        }
        // 如果失败获取数据，继续下一轮循环
        if (!got_data) {
            continue;
        }
        // 复制图像，避免修改原数据
        cv::Mat frame = data.frame.clone();  

        // 如果开启检测结果绘制
        if (config_.draw_detections) {  
            // 在图上画框、文字、时间      
            drawDetections(frame, data);      
        }

        // 如果图像尺寸与配置不一致，缩放到推流尺寸
        if (frame.cols != config_.width || frame.rows != config_.height) {
             // 使用 opencv 图像缩放函数，调整图像推流的高度、宽度、保持图像内容不变，适配推流分辨率
            cv::resize(frame, frame, cv::Size(config_.width, config_.height));
        }
        // 保存当前帧为最后一帧，用于队列为空时，重复发送最后帧，保持流不断开
        last_frame = frame.clone();  
        // 标记已该数据帧为重复发送的最后帧         
        has_last_frame = true;

        // 编码并发送到网络的成功标志
        bool success = false;
        // 需要推流则进行编码
        if (config_.enable_rtmp || config_.enable_rtsp) {   
            // 存储编码后的H264数据的动态数组，避免大小不确定导致的内存问题
            std::vector<uint8_t> encoded_frame;                        
            int packet_size = 0;
            // 编码当前帧，获取编码后的数据和大小，发送到RTMP/RTSP服务器，成功则返回 success标志
            // RK硬件编码
            if (encodeFrame(frame, encoded_frame, packet_size)) {  
                // 根据配置发送到RTMP
                if (config_.enable_rtmp) {
                    success |= writeEncodedFrame(rtmp_context_, encoded_frame.data(), packet_size, "RTMP");
                }
                // 根据配置发送到RTSP
                if (config_.enable_rtsp) {
                    success |= writeEncodedFrame(rtsp_context_, encoded_frame.data(), packet_size, "RTSP");
                }
                // 帧序号+1，用于时间戳
                frame_index_++;   
            }
        }

        // 更新推流统计（发送帧数、丢帧数、帧率）
        {
            // 加锁保护统计数据安全
            std::lock_guard<std::mutex> lock(stats_mutex_);
            // 发送成功帧计数加 1
            if (success) {
                stats_.frames_sent++;
            } else {
                // 发送失败丢帧计数加 1
                stats_.frames_dropped++;
            }
            // 计算实时帧率，每发送 config.fps 帧计算一次，避免每帧计算带来的性能开销，统计时间间隔和帧数成正比，保持统计稳定性
            auto now = std::chrono::system_clock::now();
            frame_count_++;
            // 每秒钟计算一次实时帧率
            if (frame_count_ % config_.fps == 0) {
                // 计算从上次统计到现在的时间间隔，单位毫秒
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats_.last_frame_time).count();
                // 时间间隔大于 0 才计算帧率，避免除 0 的情况发生
                if (duration > 0) {
                    // 实际帧率 = 发送帧率*1000ms / 统计时间间隔
                    stats_.fps = (config_.fps * 1000.0) / duration;
                }
                // 更新统计时间戳为当前时间，准备下一轮统计
                stats_.last_frame_time = now;
            }
        }

        // 控制发送速度，稳定在配置帧率
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / config_.fps));
    }
}

// 功能：在图像上绘制检测框、标签、时间、统计信息
void StreamingManager::drawDetections(cv::Mat& frame, const StreamingData& data) {
    // 绘制人员检测框：绿色
    for (int i = 0; i < data.person_results.count; i++) {
        // 获取当前检测结果，绘制绿色矩形框和标签，表示检测到人员，方便调试
        const auto& result = data.person_results.results[i];
        // opencv 绘制矩形框，参数，图像，左上角坐标，右下角坐标，颜色，线宽
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 255, 0), 2);
        // opencv 绘制文本，参数，图像，文本内容，位置，字体，大小，颜色，线宽
        cv::putText(frame, "Person",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
    }

    // 绘制头盔检测框：红色
    for (int i = 0; i < data.helmet_results.count; i++) {
        const auto& result = data.helmet_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 0, 255), 2);
        cv::putText(frame, "Helmet",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 1);
    }

    // 绘制疲劳检测框：黄色
    for (int i = 0; i < data.tired_results.count; i++) {
        const auto& result = data.tired_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 255, 255), 2);
        cv::putText(frame, "Tired",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
    }

    // 绘制打电话/玩手机检测框：蓝色
    for (int i = 0; i < data.callplay_results.count; i++) {
        const auto& result = data.callplay_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(255, 0, 0), 2);
        cv::putText(frame, "Call/Play",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 0), 1);
    }

    // 绘制当前系统时间
    auto now = std::chrono::system_clock::now();
    // 将时间转换为可读格式，格式转化为年月日时分秒，显示在图像左上角，方便调试和监控
    auto time_t = std::chrono::system_clock::to_time_t(now);
    // 使用字符串流格式化时间，格式为YYYY-MM-DD HH:MM:SS ，显示在图像上
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    // opencv 绘制时间文本，参数，图像，文本内容，位置，字体，大小，颜色，线宽
    cv::putText(frame, ss.str(), cv::Point(10, 30),
        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);

    // 绘制统计信息：人员/头盔/疲劳/打电话 数量
    std::string stats = "P:" + std::to_string(data.person_results.count) +
                       " H:" + std::to_string(data.helmet_results.count) +
                       " T:" + std::to_string(data.tired_results.count) +
                       " C:" + std::to_string(data.callplay_results.count);
    // opencv 绘制时间文本，参数，图像，文本内容，位置，字体，大小，颜色，线宽
    cv::putText(frame, stats, cv::Point(10, 60),
        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), 2);
}

// 功能：生成检测结果JSON字符串（可用于数据上传）
std::string StreamingManager::createDetectionJSON(const StreamingData& data) {
    // 创建一个字符串利流，用于拼接JSON字符串，最终返回完整的JSON格式字符串
    std::stringstream json;
    json << "{";
    json << "\"stream_id\":" << data.stream_id << ",";
    // 时间戳：毫秒
    json << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
        data.timestamp.time_since_epoch()).count() << ",";

    json << "\"detections\":{";
    json << "\"person\":" << data.person_results.count << ",";
    json << "\"helmet\":" << data.helmet_results.count << ",";
    json << "\"tired\":" << data.tired_results.count << ",";
    json << "\"callplay\":" << data.callplay_results.count;
    json << "}";

    json << "}";
    // 返回生成的JSON字符串
    return json.str();
}

// 功能：确保MPP硬件编码器已初始化
bool StreamingManager::ensureEncoderInitialized() {
    // 已初始化直接返回 true
    if (mpp_encoder_) {          
        return true;
    }

    // 创建编码器对象
    mpp_encoder_ = new MppEncoder();
    // 初始化解码器：宽、高、帧率、码率、H264
    if (mpp_encoder_->Init(config_.width, config_.height, config_.fps, config_.bitrate, 264) != 0) {
        // 初始化失败，打印错误信息，释放资源，指针置空，返回 false
        std::cerr << "Failed to init MPP encoder" << std::endl;
        delete mpp_encoder_;
        mpp_encoder_ = nullptr;
        return false;
    }
    // 初始化成功， 返回 true
    return true;
}

// 功能：通用FFmpeg输出初始化（RTMP/RTSP共用）
bool StreamingManager::initializeOutput(const char* format_name, const std::string& output_url, void*& opaque_context, bool use_rtsp_options) {
    // 输出 URL不能为空，检查参数有效性，避免 ffmpeg 调用失败导致崩溃
    if (output_url.empty()) {
        std::cerr << format_name << " output URL is empty" << std::endl;
        return false;
    }

    // FFmpeg 输出上下文指针，后续会分配和初始化
    AVFormatContext* fmt_ctx = nullptr;
    // 创建FFmpeg输出上下文
    if (avformat_alloc_output_context2(&fmt_ctx, nullptr, format_name, output_url.c_str()) < 0 || !fmt_ctx) {
        std::cerr << "Could not create " << format_name << " output context for " << output_url << std::endl;
        return false;
    }

    // 创建视频流
    AVStream* stream = avformat_new_stream(fmt_ctx, nullptr);
    // 填充流参数，编码格式，宽高，像素格式等，如果失败则打印错误信息，释放上下文资源， 返回 false
    if (!stream || !fillCodecParameters(stream, config_.width, config_.height)) {
        std::cerr << "Could not create stream for " << format_name << std::endl;
        avformat_free_context(fmt_ctx);
        return false;
    }

    // 设置流时间基：1/帧率
    stream->time_base = AVRational{1, config_.fps};
    // 从MPP获取SPS/PPS写入FFmpeg
    if (!attachEncoderExtradata(stream->codecpar, mpp_encoder_)) {
        avformat_free_context(fmt_ctx);
        return false;
    }
    // RTSP专用选项，TCP模式、低延迟，提升RTSP推流稳定性和实时性
    AVDictionary* options = nullptr;
    // RTSP专用设置：TCP模式、低延迟
    if (use_rtsp_options) {        
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // 设置最大延迟为100ms，减少RTSP推流延迟
        av_dict_set(&options, "muxdelay", "0.1", 0);
    }

    // 打开网络IO
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        // 使用 avio_open2 打开输出 URL，支持 RTMP、RTSP 等网络协议，失败打印错误信息，释放资源，返回 false
        if (avio_open2(&fmt_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &options) < 0) {
            std::cerr << "Could not open output URL: " << output_url << std::endl;
            // 关闭网络IO
            av_dict_free(&options);
            // 释放上下文资源
            avformat_free_context(fmt_ctx);
            return false;
        }
    }

    // 写入流头信息（FLV/RTSP头）
    if (avformat_write_header(fmt_ctx, &options) < 0) {
        std::cerr << "Error occurred when writing header to " << format_name << std::endl;
        av_dict_free(&options);
        // 关闭网络IO
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb) {
            avio_closep(&fmt_ctx->pb);
        }
        // 释放上下文资源
        avformat_free_context(fmt_ctx);
        return false;
    }
    // 释放选项字典资源
    av_dict_free(&options);
    // 保存上下文到成员变量，后续编码使用
    opaque_context = fmt_ctx;      // 保存上下文
    return true;
}

// 初始化RTMP：格式flv
bool StreamingManager::initializeRTMP() {
    return initializeOutput("flv", config_.rtmp_url, rtmp_context_, false);
}

// 初始化RTSP：格式rtsp
bool StreamingManager::initializeRTSP() {
    return initializeOutput("rtsp", config_.rtsp_url, rtsp_context_, true);
}

// 功能：调用RK MPP硬件编码器，将BGR帧编码为H264， 参数：输入帧，输出编码数据和大小，返回是否成功
bool StreamingManager::encodeFrame(const cv::Mat& frame, std::vector<uint8_t>& encoded_frame, int& packet_size) {
    if (!mpp_encoder_) {
        return false;
    }
    // MPP编码器要求输入数据必须连续，检查数据连续性，避免编码失败，打印错误信息， 返回 false
    if (!frame.isContinuous()) {   
        std::cerr << "Frame is not continuous, skip" << std::endl;
        return false;
    }

    // 编码缓冲区最大大小：宽*高*2（足够容纳H264数据），因为H264压缩后通常比原始数据小很多，但为了安全起见，预留足够空间，避免编码失败
    const int max_packet_size = config_.width * config_.height * 2;
    // 使用vector容器自动管理内存，避免手动分配和释放带来的内存泄露风险，调整大小到最大可能的编码长度
    encoded_frame.resize(max_packet_size);
    packet_size = max_packet_size;

    // 调用硬件编码。这是核心编码函数，传入原始图像数据、大小、尺寸、输出缓冲区和字节数，返回编码结果，成功则 packet_size 会更新为实际编码数据大小
    int ret = mpp_encoder_->EncodeFrame(
        frame.data,                // 图像数据
        frame.cols,                // 宽
        frame.rows,                // 高
        encoded_frame.data(),      // 编码输出缓冲区
        &packet_size,              // 输出大小
        static_cast<int>(frame.step)  // 行字节数
    );
    // 检查编码结果，编码失败或输出数据大小不合理则打印错误信息
    if (ret != 0 || packet_size <= 0) {
        return false;
    }
    // 调整vector到实际数据长度,避免后续使用时访问无效数据，确保发送正确的编码数据长度
    encoded_frame.resize(packet_size);  
    return true;
}

// 功能：将编码后的H264帧写入FFmpeg推流
bool StreamingManager::writeEncodedFrame(void* opaque_context, const uint8_t* data, int size, const char* output_name) {
    if (!opaque_context || !data || size <= 0) {
        return false;
    }

    // 获取FFmpeg上下文，检查是否有有效的流，避免写入失败导致崩溃
    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(opaque_context);
    if (fmt_ctx->nb_streams == 0) {
        return false;
    }
    // 获取视频流，准备封装数据包， 设置数据包参数，流索引、时间戳、持续时间，准备发送
    AVStream* stream = fmt_ctx->streams[0];
    AVPacket pkt = {};
    pkt.data = const_cast<uint8_t*>(data);   // 编码数据
    pkt.size = size;
    pkt.stream_index = stream->index;
    pkt.pts = frame_index_;                  // 显示时间戳
    pkt.dts = frame_index_;                  // 解码时间戳
    pkt.duration = 1;                        // 帧持续时间

    // 时间戳转换，这是FFMpeg要求的时间转换，编码器输出的时间戳是基于帧率的，需要转换为流的时间基，确保播放器正确解码和同步，避免播放异常
    AVRational src_tb{1, config_.fps};
    av_packet_rescale_ts(&pkt, src_tb, stream->time_base);

    // 发送帧数据，这是核心发送函数，api 函数将编码数据封装成流媒体格式，发送到网络服务器，失败则打印错误信息
    const int ret = av_interleaved_write_frame(fmt_ctx, &pkt);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "Error writing MPP-encoded frame to " << output_name << ": " << errbuf << std::endl;
        return false;
    }

    // 每100帧打印日志
    static int send_count = 0;
    if (++send_count % 100 == 0) {
        std::cout << "Sent frame " << send_count
                  << " to " << output_name
                  << " (MPP encoded, size=" << size << " bytes)" << std::endl;
    }

    return true;
}

// 功能：关闭推流，写文件尾，释放FFmpeg上下文
void StreamingManager::closeOutput(void*& opaque_context, const char* output_name) {
    if (!opaque_context) {
        return;
    }
    // 获取FFmpeg上下文，写入流结束标记，关闭网络IO，释放上下文资源，确保推流正确结束
    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(opaque_context);
    av_write_trailer(fmt_ctx);              // 写入流结束标记
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb) {
        avio_closep(&fmt_ctx->pb);          // 关闭网络IO
    }
    avformat_free_context(fmt_ctx);         // 释放内存
    // 输出指针置空，防止野指针
    opaque_context = nullptr;
    std::cout << output_name << " output closed" << std::endl;
}

// 功能：获取推流统计（线程安全），这是外部调用接口，返回当前的推流统计数据。包括发送帧数、丢帧数、实时帧率等
StreamingManager::StreamingStats StreamingManager::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}