# GPU Direct RDMA (GDR) Integration Guide

## Overview

GPU Direct RDMA (GDR) enables 3FS to transfer data between storage and GPU VRAM
without staging through host memory. In a conventional I/O path, data moves from
the storage target over RDMA into a host-memory buffer, then a second copy moves
it from host memory into GPU VRAM via PCIe. GDR eliminates the host bounce
buffer: the RDMA NIC writes directly into (or reads directly from) GPU VRAM in a
single DMA transaction. The result is lower latency, reduced CPU utilization, and
higher effective bandwidth for GPU-accelerated workloads such as model serving,
training checkpoint reload, and large-scale data ingest.

## Prerequisites

### Hardware

- NVIDIA GPU with CUDA compute capability (Volta or newer recommended).
- RDMA-capable network interface card (InfiniBand or RoCE) with PCIe peer access
  to the GPU. Best performance when the GPU and NIC share the same PCIe switch.

### Software

- CUDA toolkit installed and functional (`nvidia-smi` reports devices).
- The `nvidia_peermem` kernel module must be loaded:

  ```bash
  sudo modprobe nvidia_peermem
  # Verify:
  lsmod | grep nvidia_peermem
  ```

### Build

3FS must be compiled with GDR support enabled:

```bash
cmake -DHF3FS_ENABLE_GDR=ON ...
```

| Layer | Name | Meaning |
| --- | --- | --- |
| CMake option | `HF3FS_ENABLE_GDR` | Requests CUDA/GDR support at configure time. Configure fails if CUDA is unavailable. |
| C++ macro | `HF3FS_GDR_ENABLED` | Target-scoped compile definition added by `target_add_gdr_support(...)`. |
| Runtime env | `HF3FS_GDR_ENABLED=0` | Disables runtime GDR initialization in a GDR-capable build. |

Functions behind the `#ifdef HF3FS_GDR_ENABLED` guard (`hf3fs_iovopen_device`,
`hf3fs_iovwrap_device`) are only available in targets compiled with GDR support.

### Runtime

- The 3FS fuse daemon must be running on the same machine and serving the target
  mount point.
- The fuse daemon parses GDR iov symlinks (`.gdr.d{N}` suffix, `gdr://` URI
  targets) to discover GPU buffers and register them for RDMA transfers.

### CPU-Only Fallback

On machines without a GPU (or without `nvidia_peermem`),
`hf3fs_iovcreate_device()` transparently falls back to host memory allocation on
NUMA node 0. Application code that uses only `iovcreate_device` does not need
`#ifdef` guards and works on both GPU and CPU-only hosts.

## API Reference

All functions return `0` on success and `-errno` on failure unless stated
otherwise. The caller allocates the `struct hf3fs_iov` (stack or heap); the
library fills it in.

Include:

```c
#include <hf3fs_usrbio.h>
```

### Device IOV Creation

#### `hf3fs_iovcreate_device`

Allocate a new GPU memory region and register it with the fuse daemon for RDMA.
Always available (no `#ifdef` required). Falls back to host memory when GDR
runtime is not present.

```c
int hf3fs_iovcreate_device(struct hf3fs_iov *iov,
                           const char *hf3fs_mount_point,
                           size_t size,
                           size_t block_size,
                           int device_id);
```

GPU-backed iovs do not support block partitioning yet; pass `block_size = 0`.

**Returns:** `0` on success, `-EINVAL`, `-ENODEV`, `-ENOMEM`. On GDR fallback,
returns the result of host memory allocation.

---

#### `hf3fs_iovopen_device`

Reopen an existing GPU iov in a different process by its UUID. The original iov
must have been created with `hf3fs_iovcreate_device` and must still be alive.
Uses CUDA IPC to import the GPU memory handle.

**Requires `HF3FS_GDR_ENABLED` at compile time.**

```c
int hf3fs_iovopen_device(struct hf3fs_iov *iov,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);
```

`id` is the 16-byte UUID from the original `iov->id`; `size` and `device_id`
must match the original allocation. GPU-backed iovs do not support block
partitioning yet; pass `block_size = 0`.

**Returns:** `0` on success, `-ENOTSUP`, `-ENOENT`, `-ENODEV`, `-EINVAL`.

---

#### `hf3fs_iovwrap_device`

Wrap an existing GPU allocation (e.g., a PyTorch tensor's data pointer) as a 3FS
iov. The caller retains ownership of the GPU memory; `hf3fs_iovdestroy` releases
only the 3FS metadata and RDMA registration, not the underlying allocation.

**Requires `HF3FS_GDR_ENABLED` at compile time.**

```c
int hf3fs_iovwrap_device(struct hf3fs_iov *iov,
                         void *device_ptr,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);
```

`device_ptr` must point to existing GPU memory and remain valid for the iov's
lifetime. `id` is a caller-provided 16-byte UUID, unique within the mount
namespace.

GPU-backed iovs do not support block partitioning yet; pass `block_size = 0`.

**Returns:** `0` on success, `-ENOTSUP`, `-ENODEV`, `-ENOMEM`.

---

### IOV Lifecycle

#### `hf3fs_iovdestroy`

Destroy an iov and release all associated resources. Works for both host and GPU
iovs. For GPU iovs created via `iovcreate_device`, this frees the GPU memory.
For GPU iovs created via `iovwrap_device`, this releases only the 3FS metadata
and RDMA registration; the underlying GPU memory is NOT freed.

```c
void hf3fs_iovdestroy(struct hf3fs_iov *iov);
```

#### `hf3fs_iovunlink`

Remove the iov's registration symlink from the fuse namespace without
destroying the buffer. Works for both host and GPU iovs. After unlinking, the
fuse daemon will no longer accept I/O against this iov.

```c
void hf3fs_iovunlink(struct hf3fs_iov *iov);
```

---

### Synchronization

#### `hf3fs_iovsync`

Ensure coherency between GPU and NIC for a GPU iov. Internally calls
`cudaDeviceSynchronize()` on the device that owns the iov.

```c
int hf3fs_iovsync(const struct hf3fs_iov *iov, int direction);
```

`direction = 0`: GPU writes visible to NIC (call before write-submit).
`direction = 1`: NIC writes visible to GPU (call after read-complete).
No-op for host iovs. Currently maps to `cudaDeviceSynchronize()` regardless of
direction (future: stream-aware fencing).

**Returns:** `0` on success, `-EINVAL`, `-ENODEV`, `-EIO`.

---

### Capability Queries

#### `hf3fs_gdr_available`

Runtime check for GDR capability. Returns `true` only if the build includes GDR
support, the GDR manager initialized successfully, and a valid RDMA region cache
exists.

```c
bool hf3fs_gdr_available(void);
```

#### `hf3fs_gdr_device_count`

Returns the number of GPU devices visible to the GDR subsystem. Returns `0` if
GDR is not initialized.

```c
int hf3fs_gdr_device_count(void);
```

---

### Additional Utilities

#### `hf3fs_iov_mem_type`

Query the memory type of an iov.

```c
enum hf3fs_mem_type hf3fs_iov_mem_type(const struct hf3fs_iov *iov);
```

Returns `HF3FS_MEM_HOST` (0), `HF3FS_MEM_DEVICE` (1), or `HF3FS_MEM_MANAGED`
(2).

#### `hf3fs_iov_device_id`

Return the CUDA device index for a GPU iov, or `-1` for host iovs.

```c
int hf3fs_iov_device_id(const struct hf3fs_iov *iov);
```

---

### I/O Submission

The standard I/O submission functions work identically for GPU iovs:

- `hf3fs_prep_io()` -- prepare an I/O operation against a GPU iov. The `ptr`
  parameter is an offset into `iov->base` (a device pointer for GPU iovs).
- `hf3fs_submit_ios()` -- submit prepared I/Os.
- `hf3fs_wait_for_ios()` -- wait for completions.

No API changes are required in the I/O path. The fuse daemon detects GPU iovs
via their symlink metadata and routes the RDMA transfer through the GPU memory
region.

## Usage Examples

### Example A: KV Cache Reload (Inference Serving)

This example shows a complete lifecycle for loading model KV cache data from 3FS
storage directly into GPU VRAM. A 128 MB staging buffer on GPU 0 receives file
data via GDR, then the application copies it into the inference engine's working
memory with a device-to-device transfer.

```c
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cuda_runtime.h>
#include <hf3fs_usrbio.h>

#define MOUNT_POINT   "/mnt/3fs"
#define STAGING_SIZE  (128UL * 1024 * 1024)  /* 128 MB */
#define BLOCK_SIZE    0                       /* default */
#define DEVICE_ID     0
#define IO_ENTRIES    64
#define IO_TIMEOUT    30                      /* seconds */

/*
 * Helper: check 3FS API return and print message on failure.
 */
static int check_3fs(int rc, const char *context) {
    if (rc != 0) {
        fprintf(stderr, "[ERROR] %s: %s (rc=%d)\n",
                context, strerror(-rc), rc);
    }
    return rc;
}

/*
 * Reload a single KV cache shard from 3FS into GPU VRAM.
 *
 * 1. Read file data directly into the GPU staging buffer via GDR.
 * 2. D2D copy from staging buffer into the engine's working tensor.
 *
 * Returns 0 on success.
 */
static int reload_kv_shard(const struct hf3fs_ior *ior,
                           const struct hf3fs_iov *staging,
                           const char *filepath,
                           void *engine_dst,
                           size_t shard_size) {
    /* --- Open the source file and register it with the I/O ring --- */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -errno;
    }

    int rc = hf3fs_reg_fd(fd, 0);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_reg_fd failed: %d\n", rc);
        close(fd);
        return rc;
    }

    /* --- Prepare a read I/O: storage -> GPU staging buffer --- */
    /*
     * ptr is iov->base (the GPU device pointer). The fuse daemon knows
     * this is GPU memory from the .gdr symlink and will RDMA directly
     * into VRAM.
     */
    int idx = hf3fs_prep_io(ior, staging,
                            /*read=*/1,
                            staging->base,    /* destination: GPU VRAM */
                            fd,
                            /*file_offset=*/0,
                            shard_size,
                            /*userdata=*/NULL);
    if (idx < 0) {
        fprintf(stderr, "hf3fs_prep_io failed: %d\n", idx);
        hf3fs_dereg_fd(fd);
        close(fd);
        return idx;
    }

    /* --- Submit and wait for completion --- */
    rc = hf3fs_submit_ios(ior);
    if (check_3fs(rc, "hf3fs_submit_ios") != 0) {
        hf3fs_dereg_fd(fd);
        close(fd);
        return rc;
    }

    struct hf3fs_cqe cqe;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += IO_TIMEOUT;

    int completed = hf3fs_wait_for_ios(ior, &cqe, 1, 1, &deadline);
    if (completed < 0) {
        fprintf(stderr, "hf3fs_wait_for_ios failed: %d\n", completed);
        hf3fs_dereg_fd(fd);
        close(fd);
        return completed;
    }
    if (cqe.result < 0) {
        fprintf(stderr, "I/O error: %lld\n", (long long)cqe.result);
        hf3fs_dereg_fd(fd);
        close(fd);
        return (int)cqe.result;
    }

    /*
     * Sync: NIC just wrote into GPU memory. Ensure the data is visible
     * to subsequent GPU operations (direction=1: NIC -> GPU).
     */
    rc = hf3fs_iovsync(staging, /*direction=*/1);
    if (check_3fs(rc, "hf3fs_iovsync") != 0) {
        hf3fs_dereg_fd(fd);
        close(fd);
        return rc;
    }

    /*
     * Device-to-device copy from staging buffer into the engine's
     * working memory. Both pointers are GPU VRAM on the same device.
     */
    cudaError_t cerr = cudaMemcpy(engine_dst, staging->base,
                                  shard_size, cudaMemcpyDeviceToDevice);
    if (cerr != cudaSuccess) {
        fprintf(stderr, "cudaMemcpy D2D failed: %s\n",
                cudaGetErrorString(cerr));
        hf3fs_dereg_fd(fd);
        close(fd);
        return -EIO;
    }

    hf3fs_dereg_fd(fd);
    close(fd);
    return 0;
}

int main(void) {
    int rc;

    /* ========== Setup: create GPU staging buffer ========== */
    struct hf3fs_iov staging;
    rc = hf3fs_iovcreate_device(&staging, MOUNT_POINT,
                                STAGING_SIZE, BLOCK_SIZE, DEVICE_ID);
    if (check_3fs(rc, "hf3fs_iovcreate_device") != 0) {
        return 1;
    }

    /* Report what we got -- may be GPU or host fallback */
    if (hf3fs_iov_mem_type(&staging) == HF3FS_MEM_DEVICE) {
        printf("Staging buffer: GPU device %d, %zu bytes\n",
               hf3fs_iov_device_id(&staging), staging.size);
    } else {
        printf("Staging buffer: host memory fallback, %zu bytes\n",
               staging.size);
    }

    /* ========== Setup: create I/O ring ========== */
    struct hf3fs_ior ior;
    rc = hf3fs_iorcreate4(&ior, MOUNT_POINT, IO_ENTRIES,
                          /*for_read=*/1, /*io_depth=*/0,
                          IO_TIMEOUT, /*numa=*/-1, /*flags=*/0);
    if (check_3fs(rc, "hf3fs_iorcreate4") != 0) {
        hf3fs_iovdestroy(&staging);
        return 1;
    }

    /* ========== Per-request: reload a KV cache shard ========== */
    /*
     * In production, engine_mem would come from the inference framework.
     * Here we allocate a scratch buffer to demonstrate the D2D copy.
     */
    size_t shard_size = 64UL * 1024 * 1024;  /* 64 MB shard */
    void *engine_mem = NULL;
    cudaError_t cerr = cudaMalloc(&engine_mem, shard_size);
    if (cerr != cudaSuccess) {
        fprintf(stderr, "cudaMalloc engine_mem failed: %s\n",
                cudaGetErrorString(cerr));
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&staging);
        return 1;
    }

    rc = reload_kv_shard(&ior, &staging,
                         "/mnt/3fs/models/llama/kv_shard_0.bin",
                         engine_mem, shard_size);
    if (rc != 0) {
        fprintf(stderr, "Shard reload failed: %d\n", rc);
    } else {
        printf("Shard reload complete.\n");
    }

    /* ========== Shutdown ========== */
    cudaFree(engine_mem);
    hf3fs_iordestroy(&ior);
    hf3fs_iovdestroy(&staging);  /* Frees GPU staging memory */

    return rc != 0 ? 1 : 0;
}
```

---

### Example B: PyTorch Tensor Wrap

This example wraps a PyTorch-allocated GPU tensor so that 3FS can read file data
directly into it. The application retains ownership of the tensor;
`hf3fs_iovdestroy` releases only the 3FS metadata and RDMA registration.

```c
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cuda_runtime.h>
#include <hf3fs_usrbio.h>

/*
 * In a real application, this pointer and size come from PyTorch:
 *   void *tensor_ptr = tensor.data_ptr();
 *   size_t tensor_bytes = tensor.nbytes();
 *
 * For this example, we simulate with cudaMalloc.
 */

#define MOUNT_POINT  "/mnt/3fs"
#define DEVICE_ID    0
#define IO_TIMEOUT   30

/*
 * Generate a caller-defined UUID for the wrapped iov.
 * In production, use a real UUID library. This is a placeholder.
 */
static void generate_uuid(uint8_t out[16]) {
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        fread(out, 1, 16, f);
        fclose(f);
    }
    /* Set version 4 and variant bits */
    out[6] = (out[6] & 0x0F) | 0x40;
    out[8] = (out[8] & 0x3F) | 0x80;
}

int main(void) {
    int rc;

    /* --- Simulate a PyTorch tensor allocation --- */
    size_t tensor_bytes = 32UL * 1024 * 1024;  /* 32 MB */
    void *tensor_ptr = NULL;
    cudaError_t cerr = cudaSetDevice(DEVICE_ID);
    if (cerr != cudaSuccess) {
        fprintf(stderr, "cudaSetDevice failed: %s\n",
                cudaGetErrorString(cerr));
        return 1;
    }
    cerr = cudaMalloc(&tensor_ptr, tensor_bytes);
    if (cerr != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed: %s\n",
                cudaGetErrorString(cerr));
        return 1;
    }

    /* --- Wrap the tensor as a 3FS iov --- */
    /*
     * iovwrap_device registers the existing GPU allocation with the
     * RDMA subsystem and creates a symlink so the fuse daemon can
     * route I/O into this memory. The caller provides a unique UUID.
     */
    uint8_t uuid[16];
    generate_uuid(uuid);

    struct hf3fs_iov iov;
    rc = hf3fs_iovwrap_device(&iov, tensor_ptr, uuid,
                              MOUNT_POINT, tensor_bytes,
                              /*block_size=*/0, DEVICE_ID);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_iovwrap_device failed: %s (rc=%d)\n",
                strerror(-rc), rc);
        cudaFree(tensor_ptr);
        return 1;
    }

    /* --- Create an I/O ring for reads --- */
    struct hf3fs_ior ior;
    rc = hf3fs_iorcreate4(&ior, MOUNT_POINT, /*entries=*/16,
                          /*for_read=*/1, /*io_depth=*/0,
                          IO_TIMEOUT, /*numa=*/-1, /*flags=*/0);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_iorcreate4 failed: %d\n", rc);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    /* --- Read file data directly into the tensor --- */
    int fd = open("/mnt/3fs/datasets/embeddings.bin", O_RDONLY);
    if (fd < 0) {
        perror("open");
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    rc = hf3fs_reg_fd(fd, 0);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_reg_fd failed: %d\n", rc);
        close(fd);
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    int idx = hf3fs_prep_io(&ior, &iov,
                            /*read=*/1,
                            iov.base,          /* GPU device pointer */
                            fd,
                            /*file_offset=*/0,
                            tensor_bytes,
                            /*userdata=*/NULL);
    if (idx < 0) {
        fprintf(stderr, "hf3fs_prep_io failed: %d\n", idx);
        hf3fs_dereg_fd(fd);
        close(fd);
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    rc = hf3fs_submit_ios(&ior);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_submit_ios failed: %d\n", rc);
        hf3fs_dereg_fd(fd);
        close(fd);
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    struct hf3fs_cqe cqe;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += IO_TIMEOUT;

    int completed = hf3fs_wait_for_ios(&ior, &cqe, 1, 1, &deadline);
    if (completed < 0 || cqe.result < 0) {
        fprintf(stderr, "I/O failed: completed=%d, result=%lld\n",
                completed, (long long)cqe.result);
        hf3fs_dereg_fd(fd);
        close(fd);
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        cudaFree(tensor_ptr);
        return 1;
    }

    /*
     * Sync: ensure NIC writes to GPU VRAM are visible to the GPU
     * before the tensor is used in a forward pass.
     * direction=1: NIC wrote -> GPU needs to see it.
     */
    rc = hf3fs_iovsync(&iov, /*direction=*/1);
    if (rc != 0) {
        fprintf(stderr, "hf3fs_iovsync failed: %d\n", rc);
    }

    printf("Read %lld bytes directly into GPU tensor.\n",
           (long long)cqe.result);

    /* --- Cleanup --- */
    hf3fs_dereg_fd(fd);
    close(fd);
    hf3fs_iordestroy(&ior);

    /*
     * iovdestroy releases 3FS metadata and RDMA registration ONLY.
     * It does NOT free the underlying GPU memory, because this iov
     * was created with iovwrap_device (externally-owned memory).
     * The tensor remains valid for use by PyTorch.
     */
    hf3fs_iovdestroy(&iov);

    /*
     * At this point, tensor_ptr is still valid GPU memory.
     * PyTorch would continue using it (e.g., model.forward(tensor)).
     * We free it here only because this is a standalone example.
     */
    cudaFree(tensor_ptr);

    return 0;
}
```

---

### Example C: Cross-Process GPU Sharing

Two processes share a GPU buffer for I/O. Process A allocates the buffer and
passes its 16-byte UUID to Process B (via shared memory, socket, file, etc.).
Process B reopens the GPU iov by UUID and can submit independent I/O operations
against the same VRAM region.

**Important:** Process A (the exporter) must destroy the iov **last**. The
importer must call `hf3fs_iovdestroy` before the exporter does. There is no
distributed reference count -- lifetime coordination is the caller's
responsibility.

#### Process A (Exporter)

```c
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <hf3fs_usrbio.h>

#define MOUNT_POINT  "/mnt/3fs"
#define BUF_SIZE     (64UL * 1024 * 1024)  /* 64 MB */
#define DEVICE_ID    0

int main(void) {
    int rc;

    /* Allocate GPU buffer */
    struct hf3fs_iov iov;
    rc = hf3fs_iovcreate_device(&iov, MOUNT_POINT,
                                BUF_SIZE, /*block_size=*/0, DEVICE_ID);
    if (rc != 0) {
        fprintf(stderr, "iovcreate_device failed: %s (rc=%d)\n",
                strerror(-rc), rc);
        return 1;
    }

    printf("Created GPU iov on device %d, size=%zu\n",
           hf3fs_iov_device_id(&iov), iov.size);

    /*
     * Publish the 16-byte UUID so Process B can reopen this iov.
     * In production, send over a socket, write to shared memory, etc.
     * Here we write to a file for simplicity.
     */
    int uuid_fd = open("/tmp/gpu_iov_uuid.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (uuid_fd < 0) {
        perror("open uuid file");
        hf3fs_iovdestroy(&iov);
        return 1;
    }
    write(uuid_fd, iov.id, 16);
    close(uuid_fd);

    printf("UUID written to /tmp/gpu_iov_uuid.bin\n");
    printf("Waiting for Process B to finish. Press Enter to destroy iov...\n");

    /*
     * Keep the iov alive while Process B uses it.
     * Process A must destroy AFTER Process B has called iovdestroy.
     */
    getchar();

    hf3fs_iovdestroy(&iov);
    printf("GPU iov destroyed.\n");

    return 0;
}
```

#### Process B (Importer)

```c
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hf3fs_usrbio.h>

#define MOUNT_POINT  "/mnt/3fs"
#define BUF_SIZE     (64UL * 1024 * 1024)  /* Must match Process A */
#define DEVICE_ID    0
#define IO_TIMEOUT   30

int main(void) {
    int rc;

    /* Read the UUID published by Process A */
    uint8_t uuid[16];
    int uuid_fd = open("/tmp/gpu_iov_uuid.bin", O_RDONLY);
    if (uuid_fd < 0) {
        perror("open uuid file");
        return 1;
    }
    if (read(uuid_fd, uuid, 16) != 16) {
        fprintf(stderr, "Failed to read full UUID\n");
        close(uuid_fd);
        return 1;
    }
    close(uuid_fd);

    /* Reopen the GPU iov by UUID -- imports via CUDA IPC */
    struct hf3fs_iov iov;
    rc = hf3fs_iovopen_device(&iov, uuid, MOUNT_POINT,
                              BUF_SIZE, /*block_size=*/0, DEVICE_ID);
    if (rc != 0) {
        fprintf(stderr, "iovopen_device failed: %s (rc=%d)\n",
                strerror(-rc), rc);
        return 1;
    }

    printf("Reopened GPU iov: device=%d, size=%zu\n",
           hf3fs_iov_device_id(&iov), iov.size);

    /* Create I/O ring and submit reads against the shared GPU buffer */
    struct hf3fs_ior ior;
    rc = hf3fs_iorcreate4(&ior, MOUNT_POINT, /*entries=*/16,
                          /*for_read=*/1, /*io_depth=*/0,
                          IO_TIMEOUT, /*numa=*/-1, /*flags=*/0);
    if (rc != 0) {
        fprintf(stderr, "iorcreate4 failed: %d\n", rc);
        hf3fs_iovdestroy(&iov);
        return 1;
    }

    int fd = open("/mnt/3fs/data/chunk_0.bin", O_RDONLY);
    if (fd < 0) {
        perror("open data file");
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        return 1;
    }
    hf3fs_reg_fd(fd, 0);

    int idx = hf3fs_prep_io(&ior, &iov,
                            /*read=*/1, iov.base,
                            fd, /*offset=*/0,
                            BUF_SIZE, /*userdata=*/NULL);
    if (idx < 0) {
        fprintf(stderr, "prep_io failed: %d\n", idx);
        hf3fs_dereg_fd(fd);
        close(fd);
        hf3fs_iordestroy(&ior);
        hf3fs_iovdestroy(&iov);
        return 1;
    }

    hf3fs_submit_ios(&ior);

    struct hf3fs_cqe cqe;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += IO_TIMEOUT;

    int completed = hf3fs_wait_for_ios(&ior, &cqe, 1, 1, &deadline);
    if (completed > 0 && cqe.result >= 0) {
        /* Sync before GPU reads the data */
        hf3fs_iovsync(&iov, /*direction=*/1);
        printf("Read %lld bytes into shared GPU buffer.\n",
               (long long)cqe.result);
    } else {
        fprintf(stderr, "I/O failed: completed=%d, result=%lld\n",
                completed, completed > 0 ? (long long)cqe.result : 0LL);
    }

    /* Cleanup: importer must destroy before exporter */
    hf3fs_dereg_fd(fd);
    close(fd);
    hf3fs_iordestroy(&ior);
    hf3fs_iovdestroy(&iov);

    printf("Importer done. Signal Process A to destroy.\n");

    return 0;
}
```

## Runtime Behavior

### Two-Gate Model

GDR availability is determined by two independent gates:

1. **Compile-time gate** (`HF3FS_ENABLE_GDR` -> `HF3FS_GDR_ENABLED`): controls whether
   `hf3fs_iovopen_device` and `hf3fs_iovwrap_device` are declared in the header
   and compiled into the library. `hf3fs_iovcreate_device` is always compiled
   (it contains the fallback path).

2. **Runtime gate** (`hf3fs_gdr_available()`): returns `true` only when the GDR
   manager has initialized successfully and an RDMA region cache exists. This
   checks for working CUDA, `nvidia_peermem`, and IB device availability at
   runtime.

Both gates must be open for GPU I/O. If the runtime gate fails:

- `hf3fs_iovcreate_device` silently falls back to host memory on NUMA node 0.
  The caller can detect this via `hf3fs_iov_mem_type()`.
- `hf3fs_iovopen_device` and `hf3fs_iovwrap_device` return `-ENOTSUP`.

### GPU Buffer Pointer Rules

`iov->base` for a GPU iov is a CUDA device pointer. It is **not
CPU-dereferenceable**. Any attempt to read or write it from host code (memcpy,
checksum, printf of contents) will segfault. The pointer is only valid as:

- An argument to `hf3fs_prep_io()` (the fuse daemon handles it via RDMA).
- A source or destination for `cudaMemcpy()` and GPU kernel launches.

### I/O Ring Memory

The I/O submission ring (`struct hf3fs_ior`) is always allocated in host shared
memory, even when the data iov is on the GPU. The ring contains metadata (file
offsets, lengths, completion status), not bulk data. Only the data buffer
(`struct hf3fs_iov`) resides in GPU VRAM.

Host io-rings may submit I/O against GPU iovs. GPU-backed io-ring metadata is
not supported in this implementation.

### GDR Symlink Contract

The fuse daemon accepts only strict v1 GDR targets:

```text
{uuid}.gdr.d{device_id} -> gdr://v1/device/{device_id}/size/{bytes}/ipc/{128 hex chars}
```

The key device, URI device, requested API device, and requested size must match.
GDR keys reject block-size suffixes (`.b...`) and io-ring suffixes (`.r...`,
`.w...`, `.t...`, `.f...`, `.p...`).

### Automatic Skip of CPU-Side Operations

When the fuse daemon and client library detect a GPU iov:

- **Client-side CPU checksum** is automatically skipped (the CPU cannot read GPU
  memory to compute a checksum).
- **Inline data transfer** (small-I/O optimization that embeds data in the
  control message) is disabled; all transfers go through RDMA.

## Known Limitations and Future Work

- **`nvidia_peermem` required.** The current implementation depends on
  `nvidia_peermem` for RDMA memory registration of GPU buffers, not `dmabuf`.
  `hf3fs_gdr_available()` is a coarse capability check; RDMA memory registration
  (`ibv_reg_mr`) can still fail at buffer creation time if `nvidia_peermem` is
  misconfigured or the GPU/NIC combination does not support peer access.

- **Device-wide synchronization.** `hf3fs_iovsync()` maps to
  `cudaDeviceSynchronize()`, which synchronizes all streams on the device. This
  is safe but overly broad for applications that use per-stream ordering.

- **Cross-process lifetime is caller-coordinated.** There is no distributed
  reference count for shared GPU iovs. The exporting process (the one that
  called `iovcreate_device` or owns the wrapped allocation) must keep the CUDA
  allocation and iov alive until every importer has closed its iov and all RDMA
  operations using the buffer have completed. Destroying the exporter's iov
  early causes undefined behavior (stale CUDA IPC handle, potential RDMA
  errors). Fuse dead-pid cleanup removes 3FS metadata and closes importer-side
  resources; it does not make the exporting allocation safe to free.

- **GPU-NIC affinity is sysfs-based.** The current implementation selects the
  RDMA device based on sysfs PCI topology and treats the result as a best-effort
  affinity score, not a guarantee that the GPU and NIC share the optimal switch.
  A future improvement will use NVML topology queries for finer-grained PCIe
  switch distance awareness, enabling better placement when multiple NICs and
  GPUs share a complex PCIe fabric.

- **Experimental/internal import abstractions.** `AcceleratorMemoryBridge` and
  related bridge/import-manager code are not on the main GDR data path described
  above. Treat them as internal experiments unless they are wired into a specific
  caller and covered by tests.

- **Future: `dmabuf` support.** To support non-NVIDIA accelerators (AMD ROCm,
  Intel Level Zero), a `dmabuf`-based memory registration path is planned as an
  alternative to `nvidia_peermem`.

- **Future: stream-aware fencing.** Replace the device-wide
  `cudaDeviceSynchronize()` in `hf3fs_iovsync()` with CUDA event-based
  synchronization that can target a specific stream.

- **Future: GPU-side block_size partitioning.** Allow the fuse daemon to split
  large GPU iovs into block_size-aligned sub-regions for finer-grained RDMA
  scatter/gather, reducing memory waste for non-aligned I/O sizes.
