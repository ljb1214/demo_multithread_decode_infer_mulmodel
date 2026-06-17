/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#ifndef THREADPOOL_H                 // 头文件保护宏：如果未定义THREADPOOL_H，则定义它，防止重复包含
#define THREADPOOL_H                 // 定义宏，标识头文件已被包含

#include <cassert>                   // 包含断言库，用于调试检查
#include <condition_variable>        // 包含条件变量，用于线程同步
#include <functional>                // 包含std::function，用于存储可调用对象
#include <future>                    // 包含std::future和std::packaged_task，用于异步获取结果
#include <memory>                    // 包含智能指针相关（如std::shared_ptr）
#include <mutex>                     // 包含互斥锁，用于线程安全
#include <queue>                     // 包含队列容器，用于任务队列
#include <thread>                    // 包含线程库，用于创建和管理线程
#include <unordered_map>             // 包含哈希映射，用于存储线程ID到线程对象的映射
#include <atomic>                    // 包含原子类型，用于无锁的线程安全计数
#include <stdexcept>                 // 包含标准异常，如std::invalid_argument

namespace dpool                      // 定义命名空间dpool，避免符号冲突
{

    class ThreadPool                 // 定义线程池类
    {
    public:
        // 类型别名，简化代码书写
        using MutexGuard = std::lock_guard<std::mutex>;          // 互斥锁守卫（自动解锁）
        using UniqueLock = std::unique_lock<std::mutex>;         // 唯一锁（可条件等待）
        using Thread = std::thread;                              // 线程类型
        using ThreadID = std::thread::id;                        // 线程ID类型
        using Task = std::function<void()>;                      // 任务类型：无参、无返回值的可调用对象

        // 构造函数：自动检测硬件线程数，作为最大线程数
        ThreadPool() : ThreadPool(std::thread::hardware_concurrency()) {}

        // 构造函数：指定最大线程数
        explicit ThreadPool(size_t maxThreads) 
            : quit_(false),                                     // 初始化关闭标志为false
              currentThreads_(0),                               // 当前线程数初始为0
              idleThreads_(0),                                  // 空闲线程数初始为0
              maxThreads_(maxThreads > 0 ? maxThreads : 1),     // 最大线程数至少为1
              taskCount_(0),                                    // 总任务数初始为0
              completedTaskCount_(0)                            // 已完成任务数初始为0
        {
            if (maxThreads == 0) {                              // 如果传入的最大线程数为0，抛出异常
                throw std::invalid_argument("Thread count cannot be zero");
            }
        }

        // 禁用拷贝操作（确保线程池唯一性）
        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;

        // 析构函数：调用shutdown优雅关闭线程池
        ~ThreadPool()
        {
            shutdown();
        }

        // 提交任务到线程池（可变模板参数，支持任意可调用对象和参数）
        template <typename Func, typename... Ts>
        auto submit(Func &&func, Ts &&...params)
            -> std::future<typename std::result_of<Func(Ts...)>::type>   // 返回future，可获取任务结果
        {
            if (quit_.load()) {                                          // 如果线程池已关闭，抛出异常
                throw std::runtime_error("ThreadPool is shutdown");
            }

            // 使用std::bind将函数和参数绑定为一个可调用对象
            auto execute = std::bind(std::forward<Func>(func), std::forward<Ts>(params)...);
            // 推导返回类型
            using ReturnType = typename std::result_of<Func(Ts...)>::type;
            // 包装为packaged_task
            using PackagedTask = std::packaged_task<ReturnType()>;

            auto task = std::make_shared<PackagedTask>(std::move(execute));
            auto result = task->get_future();                            // 获取future，用于等待结果

            {
                MutexGuard guard(mutex_);                                 // 加锁保护任务队列
                // 将任务转换为void()类型并放入队列
                tasks_.emplace([task]() { (*task)(); });
                ++taskCount_;                                            // 总任务数加1
            }

            // 通知工作线程处理任务
            if (idleThreads_ > 0) {                                      // 如果有空闲线程，唤醒一个
                cv_.notify_one();
            } else if (currentThreads_ < maxThreads_) {                 // 否则，如果线程数未达上限，创建新线程
                createWorkerThread();
            }

            return result;                                               // 返回future
        }

        // 获取当前线程数
        size_t getCurrentThreadCount() const
        {
            MutexGuard guard(mutex_);
            return currentThreads_;
        }

        // 获取空闲线程数
        size_t getIdleThreadCount() const
        {
            MutexGuard guard(mutex_);
            return idleThreads_;
        }

        // 获取最大线程数
        size_t getMaxThreadCount() const
        {
            return maxThreads_;
        }

        // 获取待处理任务数（队列中未执行的任务）
        size_t getPendingTaskCount() const
        {
            MutexGuard guard(mutex_);
            return tasks_.size();
        }

        // 获取已完成任务数（原子操作）
        size_t getCompletedTaskCount() const
        {
            return completedTaskCount_.load();
        }

        // 获取总提交任务数
        size_t getTotalTaskCount() const
        {
            return taskCount_.load();
        }

        // 检查线程池是否已关闭
        bool isShutdown() const
        {
            return quit_.load();
        }

        // 优雅关闭线程池：等待所有线程完成当前任务后退出
        void shutdown()
        {
            {
                MutexGuard guard(mutex_);      // 加锁修改quit_标志
                quit_ = true;
            }
            cv_.notify_all();                  // 唤醒所有工作线程

            // 等待所有工作线程结束
            for (auto &elem : threads_) {
                if (elem.second.joinable()) {
                    elem.second.join();
                }
            }
            threads_.clear();                  // 清空线程映射表
            currentThreads_ = 0;               // 重置当前线程数
        }

        // 等待所有任务完成（队列空且所有线程空闲）
        void waitForAll()
        {
            UniqueLock lock(mutex_);
            cv_.wait(lock, [this]() { return tasks_.empty() && (idleThreads_ == currentThreads_); });
        }

        // 清空待处理任务队列（未执行的任务将被丢弃）
        void clearPendingTasks()
        {
            MutexGuard guard(mutex_);
            std::queue<Task> empty;            // 创建空队列
            std::swap(tasks_, empty);          // 交换，原队列被清空
        }

    private:
        // 创建工作线程
        void createWorkerThread()
        {
            Thread t(&ThreadPool::worker, this);   // 启动工作线程，绑定worker成员函数
            threads_[t.get_id()] = std::move(t);   // 存储线程对象到映射表
            ++currentThreads_;                     // 当前线程数加1
        }

        // 工作线程函数
        void worker()
        {
            while (true)
            {
                Task task;
                {
                    UniqueLock uniqueLock(mutex_);
                    ++idleThreads_;                // 进入空闲状态，空闲线程数加1
                    
                    // 等待任务或超时，或收到关闭信号
                    auto hasTimedout = !cv_.wait_for(uniqueLock,
                                                     std::chrono::seconds(WAIT_SECONDS),
                                                     [this]() {
                                                         return quit_ || !tasks_.empty();
                                                     });
                    --idleThreads_;                // 离开等待，空闲线程数减1
                    
                    if (tasks_.empty()) {          // 如果任务队列为空
                        if (quit_) {               // 如果已关闭，则退出
                            --currentThreads_;     // 当前线程数减1
                            return;
                        }
                        if (hasTimedout) {         // 如果等待超时，则退出线程（回收）
                            --currentThreads_;
                            joinFinishedThreads();             // 清理已完成的线程
                            finishedThreadIDs_.emplace(std::this_thread::get_id()); // 记录本线程ID，稍后回收
                            return;
                        }
                    }
                    
                    // 取出一个任务
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                
                // 执行任务
                try {
                    task();                                   // 调用任务函数
                    ++completedTaskCount_;                    // 已完成任务数加1
                } catch (...) {
                    // 任务执行异常，记录日志但不中断线程
                    // 这里可以添加日志记录（如std::cerr），但示例中没有实现
                }
            }
        }

        // 清理已完成的线程（回收线程资源）
        void joinFinishedThreads()
        {
            while (!finishedThreadIDs_.empty()) {
                auto id = std::move(finishedThreadIDs_.front());
                finishedThreadIDs_.pop();
                auto iter = threads_.find(id);

                if (iter != threads_.end() && iter->second.joinable()) {
                    iter->second.join();                       // 等待线程结束
                    threads_.erase(iter);                      // 从映射表中移除
                }
            }
        }

        static constexpr size_t WAIT_SECONDS = 2;              // 工作线程等待超时时间（秒）

        // 成员变量
        std::atomic<bool> quit_;                    // 关闭标志（原子操作）
        std::atomic<size_t> currentThreads_;        // 当前活跃线程数（原子）
        std::atomic<size_t> idleThreads_;           // 当前空闲线程数（原子）
        const size_t maxThreads_;                   // 最大线程数（常量）
        std::atomic<size_t> taskCount_;             // 总提交任务数（原子）
        std::atomic<size_t> completedTaskCount_;    // 已完成任务数（原子）

        mutable std::mutex mutex_;                  // 互斥锁，保护任务队列和线程相关数据
        std::condition_variable cv_;                // 条件变量，用于线程同步
        std::queue<Task> tasks_;                    // 任务队列
        std::queue<ThreadID> finishedThreadIDs_;    // 已完成线程ID队列（用于清理）
        std::unordered_map<ThreadID, Thread> threads_; // 线程ID到线程对象的映射
    };

    constexpr size_t ThreadPool::WAIT_SECONDS;      // 定义静态常量（C++17前需在类外定义）

} // namespace dpool

#endif /* THREADPOOL_H */                           // 结束头文件保护宏