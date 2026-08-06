#pragma once

#include <cstddef>
#include <cstdint>

#include "lib/api/hf3fs_usrbio.h"

#ifdef __cplusplus
extern "C" {
#endif

bool hf3fs_gdr_available(void);
int hf3fs_ensure_iov_mount_fd_internal(const char *hf3fs_mount_point);

int hf3fs_iovcreate_gpu_internal(struct hf3fs_iov *iov,
                                 const char *hf3fs_mount_point,
                                 size_t size,
                                 size_t block_size,
                                 int gpu_device_id);

int hf3fs_iovopen_gpu_internal(struct hf3fs_iov *iov,
                               const uint8_t id[16],
                               const char *hf3fs_mount_point,
                               size_t size,
                               size_t block_size,
                               int gpu_device_id);

int hf3fs_iovwrap_gpu_internal(struct hf3fs_iov *iov,
                               void *gpu_ptr,
                               const uint8_t id[16],
                               const char *hf3fs_mount_point,
                               size_t size,
                               size_t block_size,
                               int gpu_device_id);

int hf3fs_iovunlink_gpu_internal(struct hf3fs_iov *iov);
int hf3fs_iovunlink_gpu_publication_internal(const struct hf3fs_iov *iov, int device_id, bool owns_publication);
void hf3fs_iovdestroy_gpu_internal(struct hf3fs_iov *iov);
bool hf3fs_iov_is_gpu_internal(const struct hf3fs_iov *iov);
int hf3fs_iov_gpu_device_internal(const struct hf3fs_iov *iov);
int hf3fs_iovsync_gpu_internal(const struct hf3fs_iov *iov, int direction);

#ifdef __cplusplus
}
#endif
