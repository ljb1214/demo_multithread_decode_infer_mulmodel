#ifndef PROJECT2_FRAME_QUEUE_H   // 头文件保护宏：防止重复包含
#define PROJECT2_FRAME_QUEUE_H   // 定义宏，表示该头文件已被包含

#include <condition_variable>    // 包含条件变量，用于线程同步
#include <mutex>                 // 包含互斥锁，用于保护共享数据
#include <queue>                 // 包含队列容器，用于存储帧数据

template <typename T>            // 模板类，T为存储的数据类型（如帧对象）
class FrameQueue
{
public:
    // 构造函数，初始化最大容量max_size，并设置shutdown_标志为false
    explicit FrameQueue(int max_data_size) : max_size(max_data_size), shutdown_(false) {}

    // 右值引用版本的push：将临时对象移动到队列中
    bool push(T &&temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁（自动管理锁的生命周期）
        // 等待条件变量，直到队列未满或收到关闭信号
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)                              // 如果已关闭，则不再push
            return false;
        queue_.emplace(std::move(temp));            // 将temp移动构造到队列末尾
        cond_not_empty_.notify_one();               // 通知一个等待“非空”条件的线程
        return true;                                // 成功push返回true
    }

    // 左值引用版本的push：将副本放入队列（如果T是左值，将进行拷贝）
    bool push(const T &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁
        // 等待队列未满或关闭
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)                              // 已关闭则放弃push
            return false;
        queue_.emplace(temp);                       // 拷贝构造元素到队列末尾
        cond_not_empty_.notify_one();               // 唤醒等待非空的线程
        return true;
    }

    // 从队列中取出一个元素（阻塞直到有元素或关闭）
    bool wait_and_pop(T &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);   // 获取互斥锁
        // 等待队列非空或收到关闭信号
        cond_not_empty_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_ && queue_.empty())            // 如果已关闭且队列为空，则pop失败
            return false;
        temp = queue_.front();                      // 取出队首元素
        queue_.pop();                               // 弹出队首元素
        cond_not_full_.notify_one();                // 通知可能等待“非满”的线程（因为有空间了）
        return true;                                // 成功pop返回true
    }

    // 关闭队列：唤醒所有等待的线程，让它们知道关闭状态
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁（自动管理锁）
        shutdown_ = true;                           // 设置关闭标志
        cond_not_empty_.notify_all();               // 唤醒所有等待非空的线程
        cond_not_full_.notify_all();                // 唤醒所有等待非满的线程
    }

    // 返回当前队列中的元素个数
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁以保证读取安全
        return queue_.size();                       // 返回队列大小
    }

    // 清空队列，释放所有元素
    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        std::queue<T> tmp;                          // 创建一个临时空队列
        std::swap(queue_, tmp);                     // 交换，原队列被清空
        cond_not_full_.notify_all();                // 通知所有等待非满的线程（现在队列空了，有空间）
    }

    // 判断队列是否为空
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        return queue_.empty();                      // 返回是否为空
    }

    // 重置队列：设置新的最大容量，清空队列，重置关闭标志
    void reset(int new_max_size)
    {
        std::lock_guard<std::mutex> lock(mtx_);     // 加锁
        max_size = new_max_size;                    // 更新最大容量
        queue_ = std::queue<T>();                   // 清空队列（赋值空队列）
        shutdown_ = false;                          // 重置关闭标志
        cond_not_empty_.notify_all();               // 唤醒所有等待非空的线程（虽然队列已空，但通知有助于状态更新）
    }

private:
    mutable std::mutex mtx_;                        // 互斥锁，用于保护共享数据；mutable允许在const方法中加锁
    std::condition_variable cond_not_empty_;        // 条件变量，用于等待队列非空
    std::condition_variable cond_not_full_;         // 条件变量，用于等待队列非满
    size_t max_size;                                // 队列允许的最大元素个数
    bool shutdown_;                                 // 关闭标志，为true时所有阻塞操作应立即返回
    std::queue<T> queue_;                           // 底层队列容器，实际存储数据
};

#endif /* PROJECT2_FRAME_QUEUE_H */                 // 结束头文件保护宏