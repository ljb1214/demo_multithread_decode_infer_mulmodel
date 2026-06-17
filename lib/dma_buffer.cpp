#include "dma_buffer.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>      // for perror
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <mutex>        // 添加互斥锁

static std::mutex dma_mutex;   // 全局互斥锁，保护 DRM 操作

int alloc_dma_buffer(size_t size, uint8_t** out_ptr) {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，确保线程安全

    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        return -1;
    }

    struct drm_mode_create_dumb create = {};
    create.width = (uint32_t)size;
    create.height = 1;
    create.bpp = 8;
    create.flags = 0;

    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(drm_fd);
        return -1;
    }

    int fd = -1;
    struct drm_prime_handle prime_handle = {
        .handle = create.handle,
        .flags = DRM_CLOEXEC,
        .fd = -1,
    };
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) != 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        close(drm_fd);
        return -1;
    }
    fd = prime_handle.fd;

    struct drm_mode_map_dumb map = {};
    map.handle = create.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        close(fd);
        close(drm_fd);
        return -1;
    }
    void* ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, map.offset);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        close(drm_fd);
        return -1;
    }
    *out_ptr = (uint8_t*)ptr;

    close(drm_fd);
    return fd;
}

void free_dma_buffer(int fd, uint8_t* virt, size_t size) {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，保护 close/munmap 并发
    if (fd >= 0) close(fd);
    if (virt) munmap(virt, size);
}