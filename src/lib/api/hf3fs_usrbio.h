#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define HF3FS_SUPER_MAGIC 0x8f3f5fff  // hf3fs fff

typedef void *hf3fs_iov_handle;

// for data src/dst when writing to/reading from hf3fs storage
// also as the base buffer for ior
// however, if you already has a shared buffer, skip hf3fs_iovwrap() and go for hf3fs_iorwrap() directly
struct hf3fs_iov {
  uint8_t *base;
  hf3fs_iov_handle iovh;

  char id[16];
  char mount_point[256];
  size_t size;
  size_t block_size;
  int numa;
};

typedef void *hf3fs_ior_handle;

// for submitting ios to/reaping results from hf3fs fuse
struct hf3fs_ior {
  struct hf3fs_iov iov;
  hf3fs_ior_handle iorh;

  char mount_point[256];
  bool for_read;
  // io_depth > 0 to make the io worker to process io_depth ios each time
  // say when reading exactly one sample batch for training
  // == 0 to process all the prepared ios ASAP
  // notice that hf3fs_submit_ios() is just a last resort hint to the io worker
  // which may process prepared ios before they're submitted so long as it discovers them
  //  < 0 means to process ios ASAP but not more than -io_depth ios each time
  // in case there are too many ios to finish in a reasonable time
  int io_depth;
  int priority;
  int timeout;
  uint64_t flags;
};

struct hf3fs_cqe {
  int32_t index;
  int32_t reserved;
  int64_t result;
  const void *userdata;
};

// 1 for yes, 0 for no
bool hf3fs_is_hf3fs(int fd);

// returns length of the mount point + 1 if it is a valid path on an hf3fs instance
// return -1 if it is not a valid path on an hf3fs instance
// if the returned size is larger than the size in args,
// the passed-in hf3fs_mount_point buffer is not long enough
int hf3fs_extract_mount_point(char *hf3fs_mount_point, int size, const char *path);

// 0 for success, -errno for error
// iov ptr itself should be allocated by caller, it could be on either stack or heap
// the pointer hf3fs_mount_point will be copied into the corresponding field in hf3fs_iov
// it should not be too long
// numa: NUMA node hint for host memory allocation
//   >= 0: allocate on specified NUMA node
//   < 0: no NUMA binding (e.g., -1)
// For device memory (GPU etc.), use hf3fs_iovcreate_device() instead.
int hf3fs_iovcreate(struct hf3fs_iov *iov, const char *hf3fs_mount_point, size_t size, size_t block_size, int numa);
int hf3fs_iovopen(struct hf3fs_iov *iov,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa);
// GPU publication unlink failures are logged; the handle remains registered so
// hf3fs_iovunlink() or hf3fs_iovdestroy() can retry cleanup.
void hf3fs_iovunlink(struct hf3fs_iov *iov);
// iov ptr itself will not be freed
void hf3fs_iovdestroy(struct hf3fs_iov *iov);

// the iovalloc is actually creating a shm and symlink into a virtual dir in hf3fs
// so the user may want to wrap an already registered shm (for any reason)
// iov ptr itself should be allocated by caller
// the wrapped iov cannot be destroyed by hf3fs_iovdestroy()
// the underlying base ptr should not be unmapped when the iov (or its corresponding ior) is still being used
// For device memory, use hf3fs_iovwrap_device() instead.
int hf3fs_iovwrap(struct hf3fs_iov *iov,
                  void *base,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa);

// Device memory IOV creation (e.g., GPU via GDR)
// device_id: accelerator device index (0, 1, 2, ...)
// Returns -ENOTSUP if CUDA/GDR support or local CUDA IPC is unavailable;
// this API never falls back to host memory.
// A successful CUDA capability check does not guarantee RDMA registration;
// successful FUSE publication is the final per-IOV registration result.
// GDR v2 does not support block partitioning; block_size must be 0.
int hf3fs_iovcreate_device(struct hf3fs_iov *iov,
                           const char *hf3fs_mount_point,
                           size_t size,
                           size_t block_size,
                           int device_id);

#ifdef HF3FS_ENABLE_GDR
// Open an existing device memory IOV by UUID (cross-process reopen)
// The importer borrows the publisher's publication; destroy closes only the
// importer's CUDA IPC mapping and does not unlink the publication.
// Returns -ENOTSUP when local CUDA IPC is not available.
// GDR v2 does not support block partitioning; block_size must be 0.
// Only declared when HF3FS_ENABLE_GDR is defined at compile time.
int hf3fs_iovopen_device(struct hf3fs_iov *iov,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);

// Wrap externally-allocated device memory as IOV
// device_ptr may point to a subrange of a CUDA allocation and must remain valid
// until the publication and all importers are gone. CUDA IPC shares the entire
// underlying allocation even when only this subrange is published.
// Returns -ENOTSUP when local CUDA IPC is not available.
// GDR v2 does not support block partitioning; block_size must be 0.
// Only declared when HF3FS_ENABLE_GDR is defined at compile time.
int hf3fs_iovwrap_device(struct hf3fs_iov *iov,
                         void *device_ptr,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);
#endif

// calculate required memory size with wanted entries
// the calculated size can be used to create the underlying iov
size_t hf3fs_ior_size(int entries);

// ior ptr itself should be allocated by caller, on either stack or heap
int hf3fs_iorcreate(struct hf3fs_ior *ior,
                    const char *hf3fs_mount_point,
                    int entries,
                    bool for_read,
                    int io_depth,
                    int numa);
int hf3fs_iorcreate2(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int priority,
                     int numa);
int hf3fs_iorcreate3(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int priority,
                     int timeout,
                     int numa);

#define HF3FS_IOR_ALLOW_READ_UNCOMMITTED 1
#define HF3FS_IOR_FORBID_READ_HOLES 2
int hf3fs_iorcreate4(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int timeout,
                     int numa,
                     uint64_t flags);

// ior ptr itself will not be freed
void hf3fs_iordestroy(struct hf3fs_ior *ior);

// <= 0 for io-preppable file handle, errno for error
// fd has to be registered before used in hf3fs_prep_io()
// registered fds should not be closed, and even if it's closed, the old inode will still be used to prep io
// also, if a registered fd is closed, and a new fd with the same integer value is to be registered
// the registration will fail with an EINVAL
int hf3fs_reg_fd(int fd, uint64_t flags);
void hf3fs_dereg_fd(int fd);

// report max number of entries in the ioring
int hf3fs_io_entries(const struct hf3fs_ior *ior);

// >= 0 for io index, -errno for error
// this functioon is *NOT* thread safe!!!!!
// do not prepare io in the same ioring from different threads
// or the batches may be mixed and things may get ugly for *YOU*
// with such assumption, we don't waste time for the thread-safety
int hf3fs_prep_io(const struct hf3fs_ior *ior,
                  const struct hf3fs_iov *iov,
                  bool read,
                  void *ptr,
                  int fd,
                  size_t off,
                  uint64_t len,
                  const void *userdata);
// 0 for success, -errno for error
int hf3fs_submit_ios(const struct hf3fs_ior *ior);
// >= 0 for result count, -errno for error, may return fewer than ready, call again to make sure
int hf3fs_wait_for_ios(const struct hf3fs_ior *ior,
                       struct hf3fs_cqe *cqes,
                       int cqec,
                       int min_results,
                       const struct timespec *abs_timeout);

int hf3fs_hardlink(const char *target, const char *link_name);
int hf3fs_punchhole(int fd, int n, const size_t *start, const size_t *end, size_t flags);

enum hf3fs_mem_type {
  HF3FS_MEM_HOST = 0,
  HF3FS_MEM_DEVICE = 1,
  HF3FS_MEM_MANAGED = 2,
};

// Reports application-local CUDA IPC/device capability only. It does not prove
// that FUSE can import the allocation or register an RDMA MR; successful
// create/wrap publication is the final per-IOV result.
bool hf3fs_gdr_available(void);
// Returns the number of locally visible CUDA devices; it does not probe FUSE RDMA registration.
int hf3fs_gdr_device_count(void);
enum hf3fs_mem_type hf3fs_iov_mem_type(const struct hf3fs_iov *iov);
int hf3fs_iov_device_id(const struct hf3fs_iov *iov);
int hf3fs_iovsync(const struct hf3fs_iov *iov, int direction);

#ifdef __cplusplus
}
#endif
