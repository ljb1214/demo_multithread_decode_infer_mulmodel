#ifndef __MEMORY_POOL_HPP__           // 头文件保护宏：如果未定义__MEMORY_POOL_HPP__，则定义它，防止重复包含
#define __MEMORY_POOL_HPP__           // 定义该宏，表示头文件已被包含

#include <mutex>                      // 包含互斥锁头文件，用于线程同步
#include <queue>                      // 包含队列容器，用于管理空闲内存块
#include <vector>                     // 包含向量容器（虽然此处未使用，但可能是预留）

class MemoryPool {                    // 内存池类，用于高效分配固定大小的内存块
public:
    // 构造函数：指定每个内存块的大小和总块数
    MemoryPool(size_t block_size, size_t block_count) 
        : block_size_(block_size), block_count_(block_count) {
        // 预分配内存：分配 block_size * block_count 字节的连续内存
        buffer_ = new uint8_t[block_size * block_count];
        // 初始化空闲块列表：将每个块的起始地址压入队列
        for (size_t i = 0; i < block_count; i++) {
            free_blocks_.push(buffer_ + i * block_size);
        }
    }
    
    // 析构函数：释放预分配的内存
    ~MemoryPool() {
        delete[] buffer_;            // 删除整个缓冲区
    }
    
    // 分配一个内存块
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);  // 加锁保护共享资源
        if (free_blocks_.empty()) {                // 如果没有空闲块
            return nullptr;                        // 返回空指针（或可在此实现扩容逻辑）
        }
        void* block = free_blocks_.front();        // 取出队列头部的空闲块地址
        free_blocks_.pop();                        // 从空闲队列中移除该块
        return block;                              // 返回块地址供使用
    }
    
    // 归还一个内存块
    void deallocate(void* block) {
        std::lock_guard<std::mutex> lock(mutex_);  // 加锁保护共享资源
        free_blocks_.push(block);                  // 将块地址重新放入空闲队列
    }

private:
    size_t block_size_;                // 每个内存块的大小（字节数）
    size_t block_count_;               // 内存池中块的总数
    uint8_t* buffer_;                  // 指向预分配内存缓冲区的指针
    std::queue<void*> free_blocks_;    // 空闲块地址队列（存放可用的块指针）
    std::mutex mutex_;                 // 互斥锁，保证多线程下对空闲队列的安全访问
};

#endif /* __MEMORY_POOL_HPP__ */       // 结束头文件保护宏