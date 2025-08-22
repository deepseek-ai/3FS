# USRBIO API Reference

## Overview
User Space Ring Based IO, or USRBIO, is a set of high-speed I/O functions on 3FS. User applications can directly submit I/O requests to the 3FS I/O queue in the FUSE process via the USRBIO API, thereby bypassing certain limitations inherent to FUSE itself. For example, this approach avoids the maximum single I/O size restriction, which is notoriously unfriendly to network file systems. It also makes the data exchange between the user and FUSE processes.

## Concepts
**Iov**: A large shared memory region for zero-copy read/write operations, shared between the user and FUSE processes, with InfiniBand (IB) memory registration managed by the FUSE process. In the USRBIO API, all read data will be read into Iov, and all write data should be written to Iov by user first.

**Ior**: A small shared memory ring for communication between user process and FUSE process. The usage of Ior is similar to Linux [io-uring](https://unixism.net/loti/index.html), where the user application enqueues read/write requests, and the FUSE process dequeues these requests for completion. The I/Os are executed in batches controlled by the `io_depth` parameter, and multiple batches will be executed in parallel, be they from different rings, or even from the same ring. However, multiple rings are still recommended for multi-threaded applications, as synchronization is unavoidable when sharing a ring.

**File descriptor Registration**: Functions are provided for file descriptor registration and deregistration. Only registered fds can be used for the USRBIO. The file descriptors in the user application are managed by the Linux kernel and the FUSE process has no way to know how they're actually associated with inode IDs it manages. The registration makes the I/O preparation function look more like the [uring counterpart](https://unixism.net/loti/ref-liburing/submission.html).

## Functions

### hf3fs_iorcreate4

#### Summary
Create an Ior instance. All `hf3fs_iorcreate*` functions create Ior instances, but include various configurable parameters due to compatibility considerations. The `struct hf3fs_ior` instance can be allocated on stack as a local variable or as a member field of another struct. The create functions will not allocate memory for it, and the destroy function will not deallocate. The `struct hf3fs_iov` is the same.

#### Syntax
```c
int hf3fs_iorcreate4(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int timeout,
                     int numa,
                     uint64_t flags);
```

#### Parameters
- **ior**: Address for `hf3fs_ior`.
- **hf3fs_mount_point**: Mount point for 3FS. This parameter is used to distinguish 3FS clusters, enabling a single machine to mount multiple 3FS instances.
- **entries**: Maximum number of concurrent read/write requests that can be submitted.
- **for_read**: `true` if this Ior handles read requests, `false` if this Ior handles write requests. An Ior cannot handle read requests and write requests simultaneously.
- **io_depth**: `0` for no control with I/O depth. If greater than 0, then only when `io_depth` I/O requests are in queue, they will be issued to server as a batch. If smaller than 0, then USRBIO will wait for at most `-io_depth` I/O requests are in queue and issue them in one batch. 
- **timeout**: Maximum wait time for batching when `io_depth` < 0.
- **numa**: Numa ID for Ior shared memory. `-1` for current process numa ID.
- **flags**: A flag composed of OR-ed bits to specify special behaviors.

#### Return Value
- If success, return 0.
- If fail, return `-errno`.

#### Example
```c
struct hf3fs_ior ior;
hf3fs_iorcreate4(&ior, "/hf3fs/mount/point", 1024, true, 0, 0, -1, 0);
hf3fs_iordestroy(&ior);
```

### hf3fs_iordestroy

#### Summary
Destroy an Ior.

#### Syntax
```c
void hf3fs_destroy(struct hf3fs_ior *ior);
```

#### Parameters
- **ior**: Address for Ior.

### hf3fs_iovcreate

#### Summary
Create an Iov instance and allocate shared memory for that Iov.

#### Syntax
```c
int hf3fs_iovcreate(struct hf3fs_iov *iov,
                    const char *hf3fs_mount_point,
                    size_t size,
                    size_t block_size,
                    int numa);
```

#### Parameters
- **iov**: Address for Iov.
- **hf3fs_mount_point**: Mount point for 3FS. This parameter is used to distinguish 3FS clusters, enabling a single machine to mount multiple 3FS instances.
- **size**: Shared memory size for this Iov.
- **block_size**: If not `0`, this function will allocate multiple shared memory blocks, each sized no larger than `block_size`. `0` for allocate a single large shared memory. All IOs on this Iov should not span across the block margin. This parameter is for optimization on IB register time.
- **numa**: Numa ID for Ior shared memory. `-1` for current process numa ID.

#### Return Value
- If success, return 0.
- If fail, return `-errno`.

#### Example
```c
struct hf3fs_iov iov;
hf3fs_iovcreate(&iov, "/hf3fs/mount/point", 1 << 30, 0, -1);
hf3fs_iovdestroy(&iov);
```

### hf3fs_iovdestroy

#### Summary
Destroy an Iov.

#### Syntax
```c
void hf3fs_iovdestroy(struct hf3fs_iov *iov);
```

#### Parameters
- **param**: Address for Iov.

### hf3fs_reg_fd

#### Summary
Register a file descriptor for FUSE IO.

#### Syntax
```c
int hf3fs_reg_fd(int fd, uint64_t flags);
```

#### Parameters
- **fd**: A Linux file descriptor.
- **flags**: Unused. For future use.

#### Return Value
- If success, return an integer less or equal than 0. This integer can be used in `hf3fs_prep_io` as `fd`. You can view this as an extra `fd` which is only usable in USRBIO API, and `hf3fs_prep_io` will accept both this new `fd` or the original Linux `fd`.
- If fail, return `errno`.

### hf3fs_dereg_fd

#### Summary
Deregister a file descriptor.

#### Syntax
```c
void hf3fs_dereg_fd(int fd);
```

#### Parameters
- **fd**: A Linux file descriptor.

#### Example
```c
int fd = open("example.txt", O_RDONLY);
hf3fs_reg_fd(fd, 0);
hf3fs_dereg_fd(fd);
close(fd);
```

### hf3fs_prep_io

#### Summary
Submit an I/O request to an Ior.

#### Syntax
```c
int hf3fs_prep_io(struct hf3fs_ior *ior,
                  const struct hf3fs_iov *iov,
                  bool read,
                  void *ptr,
                  int fd,
                  size_t off,
                  uint64_t len,
                  void *userdata);
```

#### Parameters
- **ior**: Address for Ior.
- **iov**: Address for Iov.
- **read**: `true` for read, `false` for write. Must match the Ior create parameters.
- **ptr**: The address for I/O operation. `[ptr, ptr + len)` must be fully in the range provided by the Iov.
- **fd**: File for I/O operation. Must be registered by `hf3fs_reg_fd`.
- **off**: Offset in file.
- **len**: Read size or write size.
- **userdata**: Arbitrary data which will returned by `hf3fs_wait_for_ios`.

#### Return Value
- If success, return the index of I/O request in the Ior.
- If fail, return `-errno`.

#### Notes
- This function may not be thread safe.

### hf3fs_submit_ios

#### Summary
Notify FUSE process that new I/O operations has been submitted.

#### Syntax
```c
int hf3fs_submit_ios(const struct hf3fs_ior *ior);
```

#### Parameters
- **ior**: Address for Ior.

#### Return Value
- If success, return 0.
- If fail, return `-errno`.

#### Notes
- The I/O operations may be executed **before** you call `hf3fs_submit_ios`. This function is just notifying FUSE process to work, but the FUSE process also scan new operations periodically.

### hf3fs_wait_for_ios

#### Summary
Wait and get results for completed I/O operations.

#### Syntax
```c
int hf3fs_wait_for_ios(const struct hf3fs_ior *ior,
                       struct hf3fs_cqe *cqes,
                       int cqec,
                       int min_results,
                       const struct timespec *abs_timeout);
```

#### Parameters
- **ior**: Address for Ior.
- **cqes**: Address for `hf3fs_cqe`s. This will contains I/O operation result, and `userdata` provided by `hf3fs_prep_io`.
- **cqec**: The size of array pointed by `cqes`.
- **min_results**: Minimum number of results to return.
- **abs_timeout**: Maximum timeout to return.

#### Return Value
- If success, return number of completed I/O requests.
- If fail, return `-errno`.

#### Example
```c
hf3fs_prep_io(&ior, &iov, true, iov.base, fd, 0, 4096, nullptr);
hf3fs_prep_io(&ior, &iov, true, iov.base + 4096, fd, 4096, 4096, nullptr);
hf3fs_submit_ios(&ior);

hf3fs_cqe cqes[2];
hf3fs_wait_for_ios(&ior, cqes, 2, 2, nullptr);
```

#### Notes
- It is OK to call `hf3fs_prep_io` and `hf3fs_submit_ios` in one thread, and call `hf3fs_wait_for_ios` in another thread. But only one thread can call `hf3fs_prep_io` and `hf3fs_submit_ios`, and only one thread can call `hf3fs_wait_for_ios`.

## Example

```c
#include <hf3fs_usrbio.h>

constexpr uint64_t NUM_IOS = 1024;
constexpr uint64_t BLOCK_SIZE = (32 << 20);

int main() {
    struct hf3fs_ior ior;
    hf3fs_iorcreate4(&ior, "/hf3fs/mount/point", NUM_IOS, true, 0, 0, -1, 0);
    
    struct hf3fs_iov iov;
    hf3fs_iovcreate(&iov, "/hf3fs/mount/point", NUM_IOS * BLOCK_SIZE, 0, -1);

    int fd = open("/hf3fs/mount/point/example.bin", O_RDONLY);
    hf3fs_reg_fd(fd, 0);

    for (int i = 0; i < NUM_IOS; i++) {
        hf3fs_prep_io(&ior, &iov, true, iov.base + i * BLOCK_SIZE, fd, i * BLOCK_SIZE, BLOCK_SIZE, nullptr);
    }
    hf3fs_submit_ios(&ior);

    hf3fs_cqe cqes[NUM_IOS];
    hf3fs_wait_for_ios(&ior, cqes, NUM_IOS, NUM_IOS, nullptr);

    hf3fs_dereg_fd(fd);
    close(fd);
    hf3fs_iovdestroy(&iov);
    hf3fs_iordestroy(&ior);

    return 0;
}
```
