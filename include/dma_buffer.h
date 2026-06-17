#ifndef DMA_BUFFER_H
#define DMA_BUFFER_H

#include <cstddef>
#include <cstdint>
#include "dma_buffer.h"
#include <unistd.h> // for close
#include <sys/mman.h> // for munmap
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// 分配 dma-buf，返回 fd，并通过 out_ptr 返回映射的虚拟地址（可选，可为 NULL）
int alloc_dma_buffer(size_t size, uint8_t** out_ptr);
void free_dma_buffer(int fd, uint8_t* virt, size_t size);

#ifdef __cplusplus
}
#endif

#endif