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
int hf3fs_iovcreate(struct hf3fs_iov *iov, const char *hf3fs_mount_point, size_t size, size_t block_size, int numa);
int hf3fs_iovopen(struct hf3fs_iov *iov,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa);
void hf3fs_iovunlink(struct hf3fs_iov *iov);
// iov ptr itself will not be freed
void hf3fs_iovdestroy(struct hf3fs_iov *iov);

// the iovalloc is actually creating a shm and symlink into a virtual dir in hf3fs
// so the user may want to wrap an already registered shm (for any reason)
// iov ptr itself should be allocated by caller
// the wrapped iov cannot be destroyed by hf3fs_iovdestroy()
// the underlying base ptr should not be unmapped when the iov (or its corresponding ior) is still being used
int hf3fs_iovwrap(struct hf3fs_iov *iov,
                  void *base,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa);

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
#ifdef __cplusplus
}
#endif
