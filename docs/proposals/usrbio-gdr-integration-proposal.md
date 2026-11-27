# Proposal: GPU Direct RDMA (GDR) for 3FS usrbio

- **Status:** Draft
- **Authors:** @SimonCqk
- **Last Updated:** 2026-03-17

---

## 1. Motivation

In current implementation, when GPU workloads need to read or write data through 3FS, they must bounce through host buffers — the storage service transfers data to host RAM via RDMA, and the application then copies it to GPU memory (or vice versa for writes). This host bounce adds latency, consumes CPU memory bandwidth, and becomes a throughput bottleneck for large sequential reads and medium random reads common in model training and inference.

GPU Direct RDMA (GDR) eliminates this bounce by allowing read from and write to GPU VRAM (or other accelerator device memory in the future) directly over PCIe. With GDR, data moves on a single path — storage server → RDMA → GPU VRAM — with zero CPU-side copies.

This proposal introduces GDR as a first-class capability, defining API contracts, registration flows, lifecycle ordering, and data paths for GPU memory through.

---

## 2. Goals and Non-Goals

### Goals

1. Extend usrbio with explicit `_device` APIs for GPU (or other accelerator devices) memory alongside the original host API, sharing lifecycle operations (`iovunlink`, `iovdestroy`) across both paths.
2. Enable the fuse daemon and storage client RDMA data path to transfer data directly to/from accelerator VRAM, bypassing host memory bounce buffers.
3. Define deterministic compile-time and runtime fallback behavior for CPU-only environments.
4. Specify the complete RDMA memory registration design for GPU buffers, including cross-process IPC import.
5. Support both single-process (app allocates + does I/O) and cross-process (app allocates, fuse daemon does I/O) deployment models.

### Non-Goals

1. Non-CUDA accelerator backends (AMD ROCm, Intel Level Zero, etc.) for now.
2. Guaranteed performance parity with host path for all workload shapes.

---

## 3. Design

### 3.1 API

This proposal extends the existing usrbio C API surface with explicit `_device` variants for accelerator memory (GPU today, extensible to other device types), keeping the original host API unchanged and backward compatible. Device intent is expressed through separate entry points with a `device_id` parameter:

```c
// Host memory: original API, unchanged
hf3fs_iovcreate(&iov, mount, size, block_size, /*numa=*/0);   // NUMA node 0

// Device memory: explicit device id specify
hf3fs_iovcreate_device(&iov, mount, size, block_size, /*device_id=*/0);  // GPU 0
```

The host lifecycle functions — `hf3fs_iovcreate`, `hf3fs_iovopen`, `hf3fs_iovwrap` — retain their original semantics. The device API provides three parallel entry points:

- `hf3fs_iovcreate_device(iov, mount, size, block_size, device_id)` — always available; falls back to host memory if GDR is unavailable.
- `hf3fs_iovopen_device(iov, id, mount, size, block_size, device_id)` — only available when `HF3FS_GDR_ENABLED` is defined at compile time.
- `hf3fs_iovwrap_device(iov, device_ptr, id, mount, size, block_size, device_id)` — only available when `HF3FS_GDR_ENABLED` is defined at compile time.

**Fallback behavior:** Two independent gates govern whether the GPU path is actually taken. At compile time, `#ifdef HF3FS_GDR_ENABLED` strips all GPU code when OFF — `iovopen_device` and `iovwrap_device` are not even declared in the header. At runtime, `hf3fs_gdr_available()` means the GDR manager initialized successfully and a GPU memory-region cache is available. Lower-level failures such as verbs registration or driver/`nvidia_peermem` issues may still surface during buffer creation or first-use registration in the fuse daemon.

---

## 4. Architecture

### 4.1 Process Model

3FS GPU I/O always involves two OS processes — the application and the fuse daemon. The diagram below shows the control plane (buffer publication and import) alongside the data plane (RDMA transfer):

```
┌─────────────── Application Process ───────────────┐
│                                                     │
│  usrbio _device API                                 │
│  ┌───────────────────────────────────────────┐      │
│  │ iovcreate_device / iovwrap_device         │      │
│  │   → cudaMalloc or wrap existing devicePtr │      │
│  │   → export cudaIpcMemHandle               │      │
│  │   → publish .gdr symlink to namespace     │      │
│  └───────────────────────────────────────────┘      │
│                                                     │
│  Submit I/O via host shared-memory ring             │
│  (hf3fs_prep_io → hf3fs_submit_ios)                │
└─────────────────────┬───────────────────────────────┘
                      │  namespace + SHM ring
                      ▼
┌─────────────── Fuse Daemon Process ───────────────┐
│                                                     │
│  Import GPU buffer                                  │
│  ┌───────────────────────────────────────────┐      │
│  │ parse .gdr URI → cudaIpcOpenMemHandle     │      │
│  │   → obtain daemon-local devicePtr         │      │
│  │   → ibv_reg_mr on GPU memory (daemon MR)  │      │
│  └───────────────────────────────────────────┘      │
│                                                     │
│  Dispatch I/O                                       │
│  ┌───────────────────────────────────────────┐      │
│  │ UUID → GpuShmBufForIO → RDMABufUnified    │      │
│  │ StorageClient builds RDMA request         │      │
│  │   (no CPU dereference on GPU ptr)         │      │
│  └──────────────────┬────────────────────────┘      │
└─────────────────────┼───────────────────────────────┘
                      │  RDMA READ / WRITE
                      ▼
              ┌───────────────┐        ┌────────────────┐
              │  HCA / NIC    │◄──────►│ Storage Server  │
              │  nvidia_peermem│        │ disk / cache    │
              │  + GPU MR     │        └────────────────┘
              └───────┬───────┘
                      │ PCIe (zero-copy)
                      ▼
              ┌───────────────┐
              │  GPU VRAM     │
              └───────────────┘
```

The two processes communicate through:
1. **Filesystem namespace:** symlinks in `/mount/3fs-virt/iovs/` carry buffer metadata (UUID, device ID, IPC handle).
2. **CUDA IPC:** the 64-byte `cudaIpcMemHandle_t` is hex-encoded in the symlink target URI.
3. **Shared memory ring:** the I/O ring (`IoArgs`, `IoSqe`, `IoCqe`) lives in POSIX SHM for submission/completion.

### 4.2 Key Data Structures

#### Application side

- `hf3fs_iov` remains the user-visible object for both host and GPU (accelerator) buffers. `iov->base` is either an `mmap` host pointer or a device pointer.
- `GpuIovHandle` carries GPU-local state: `devicePtr`, `deviceId`, ownership flags, exported `cudaIpcMemHandle_t`, and the process-local RDMA registration.
- `GDRManager` owns runtime capability detection and GPU-to-IB affinity discovery.
- `AcceleratorMemoryRegionCache` caches MR registrations by `devicePtr` to avoid repeated `ibv_reg_mr` calls.

#### Fuse daemon side

- `GpuShmBuf` holds an imported GPU buffer: device pointer, `deviceId`, size, and a daemon-local `AcceleratorMemoryRegion`.
- `GpuShmBufForIO` is an offsetted view over `GpuShmBuf` for a single I/O operation; `ptr()` returns a GPU address (not CPU-dereferenceable) and `memh()` produces an `IOBuffer` backed by `RDMABufUnified(Gpu)`.
- `RDMABufAccelerator` is the GPU RDMA wrapper providing address, length, and `getMR()` for building verbs requests.

**Invariant:** `iov->iovh` is polymorphic — for host buffers it points to `ShmBuf*`, for GPU buffers it points to `GpuIovHandle*`. The runtime discriminant is `iov->numa == kGpuIovMagicNuma` combined with presence in the global `gGpuIovHandles` map.

---

## 5. Implementation

### 5.1 GDR Action Chain & Metadata Contract

A GPU buffer flows through the following chain: the application calls a `_device` API entry point (`UsrbIo.cc`), which delegates to `UsrbIoGdr.cc` for GPU buffer creation, CUDA IPC export, and local MR registration via `AcceleratorMemory`. The buffer is then published as a fuse namespace symlink with filename `{uuid}.gdr.d{device_id}` pointing to a URI that encodes device ID, size, and the hex-encoded 64-byte CUDA IPC handle:

```
gdr://v1/device/{device_id}/size/{size}/ipc/{hex-encoded-64-byte-handle}
```

On the fuse daemon side, `IovTable` parses this URI, imports the CUDA IPC handle, and creates a daemon-local MR. During I/O dispatch, `IoRing` / `FuseClients` resolve the UUID to a `GpuShmBufForIO`, which is carried through the submission ring and ultimately consumed by `RDMABufUnified` — the uniform RDMA abstraction used by `IBSocket` / `StorageClient` to issue `ibv_post_send`.

The `hf3fs_iov` struct is shared across host and GPU paths — GDR reuses the same fields with different semantics:

```c
struct hf3fs_iov {
  uint8_t *base;           // Host: mmap host pointer
                           // GPU:  CUDA device pointer (NOT CPU-dereferenceable)
  hf3fs_iov_handle iovh;   // Host: ShmBuf*
                           // GPU:  GpuIovHandle* (carries deviceId, IPC handle, local MR)
  char id[16];             // 16-byte UUID, same for both paths
  char mount_point[256];
  size_t size;
  size_t block_size;
  int numa;                // Host: NUMA node (>= 0) or no binding (< 0)
                           // GPU:  kGpuIovMagicNuma (-0x6472), runtime type discriminant
};
```

The I/O submission ring (`hf3fs_iorcreate*`) remains host shared memory regardless of the data iov type.

### 5.2 Coherency Model

GPU GDR buffers are shared between CUDA execution engines and the RDMA NIC, not between CPU threads and the NIC.

- A successful CQE from `hf3fs_wait_for_ios()` means the RDMA operation is complete: for reads the GPU memory has been written by the NIC; for writes the GPU memory has been consumed.
- `iov->base` for a GPU iov is **not** CPU-addressable — CPU `memcpy`, inline payload construction, and checksum calculation are all invalid on the GPU path.
- `hf3fs_iovsync(iov, direction)` provides a conservative fence: `direction = 0` makes preceding GPU writes visible to the NIC before RDMA; `direction = 1` makes NIC writes visible before dependent GPU work. Both currently map to `cudaDeviceSynchronize()` — correct but coarse and not stream-aware. In many deployments `hf3fs_wait_for_ios()` alone is sufficient for post-read consumption.

### 5.3 Read Path (Storage → GPU)

When the application submits a read I/O against a GPU iov, the fuse daemon dispatches it through the GPU data path:

1. The application prepares the read with `hf3fs_prep_io(...)` and submits it through the host-resident I/O ring.
2. The fuse daemon wakes up, processes the SQE, and resolves the target UUID through `lookupBufs(uuid)`.
3. For a GPU iov, the host-shm lookup misses and `gpuShmsById` returns a `GpuShmBufForIO` view over the imported GPU allocation.
4. The daemon derives a GPU device address through `ptr()` and a GPU-capable `IOBuffer` through `memh()`, which exposes the correct GPU MR and `rkey` state.
5. `StorageClient::read(...)` and the RDMA batch builder treat that buffer through `RDMABufUnified(Gpu)`, so no host dereference or CPU copy is required.
6. `ibv_post_send(RDMA READ)` causes the HCA to write file data directly into GPU VRAM over PCIe via `nvidia_peermem`.
7. After `hf3fs_wait_for_ios()` reports completion, the GPU memory range contains the requested file data.

The critical insight is unchanged: the fuse daemon orchestrates the operation but does not dereference the data. The HCA performs the PCIe transfer into VRAM directly.

### 5.4 Write Path (GPU → Storage)

The write path is symmetric to the read path. The application submits a write against a GPU iov, the fuse daemon resolves it to the same `GpuShmBufForIO` + `RDMABufUnified(Gpu)` abstraction, and issues an `ibv_post_send(RDMA WRITE)`. The HCA reads directly from GPU VRAM and transmits to the storage server with zero CPU-side copies.

The one extra requirement is producer ordering: if the GPU buffer was just written by CUDA kernels, the caller should ensure those writes are visible before submit. In the current API that conservative fence is `hf3fs_iovsync(&iov, 0)`.

### 5.5 StorageClient Changes for Device Buffers

Adding device MRs alone is not enough. The StorageClient path had several implicit host-memory assumptions that had to be removed. The key changes are:

**1. `IOBuffer` wraps `RDMABufUnified` instead of host-only `RDMABuf`.**

```cpp
class IOBuffer {
  net::RDMABufUnified rdmabuf_;              // was: net::RDMABuf
  bool isDeviceMemory() const { return rdmabuf_.isDevice(); }
};
```

**2. Inline read/write data is disabled for device buffers** — CPU `memcpy` into/from a device pointer is invalid.

```cpp
// Inline data only for host buffers
if (bytes < max_inline_read_bytes && !hasDeviceBuffer) {
  BITFLAGS_SET(featureFlags, FeatureFlags::SEND_DATA_INLINE);
}
```

**3. Client-side CPU checksum is skipped for device buffers** — checksum responsibility stays on the storage server side.

```cpp
if (options.verifyChecksum() && !readIO->buffer->isDeviceMemory()) {
  auto checksum = ChecksumInfo::create(type, readIO->data, length);
  // ... verify ...
}
```

These changes preserve the storage protocol while removing accidental CPU touches on device memory.

### 5.6 CUDA IPC and Cross-Process Memory Sharing

The application process allocates GPU memory and must share it with the fuse daemon, which runs as a separate OS process. CUDA IPC provides the mechanism.

**Export (application side).** `cudaIpcGetMemHandle(&handle, devicePtr)` produces a 64-byte opaque handle that encodes enough information for another process on the same machine to map the same GPU allocation.

**Transport.** The IPC handle is hex-encoded (128 hex characters) and embedded in the fuse symlink target URI:

```
gdr://v1/device/0/size/1073741824/ipc/aabb...ff00
```

This piggybacks on the existing fuse namespace — no additional control-plane protocol is needed for the primary path.

**Import (fuse daemon side).** When `IovTable::addIov` encounters a `.gdr` key, it parses the URI, extracts the IPC handle bytes, and calls `cudaIpcOpenMemHandle(&importedPtr, cudaHandle, cudaIpcMemLazyEnablePeerAccess)`. The returned `importedPtr` is a valid device pointer in the daemon's CUDA address space, backed by the same physical GPU memory.

**Dual-side MR registration.** Both processes independently call `ibv_reg_mr` on their respective device pointers. This is required because IB memory regions are per-process. The daemon-side MR is the one used for actual storage RDMA operations; exporter-side and opener-side MRs are local registrations owned by those processes.

---

## 6. Use Cases

### A. KV cache reload (inference serving)

```c
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <cuda_runtime.h>

#include "hf3fs_usrbio.h"

/*
 * Typical inference serving pattern:
 *   1. At startup, allocate a fixed GPU staging buffer via iovcreate_device.
 *   2. On each request, load the needed KV cache shard from 3FS into the
 *      staging buffer via GDR (storage → RDMA → GPU VRAM), then D2D-copy
 *      into the engine's own managed memory (e.g. paged attention pool).
 *   3. The staging iov is reused across requests — no repeated allocation.
 */

static const size_t kStagingBytes = 128ULL << 20;  /* 128 MB staging buffer */

/* --- startup: one-time iov + ior setup --- */

struct hf3fs_iov staging_iov;
hf3fs_iovcreate_device(&staging_iov, "/mnt/3fs", kStagingBytes, 0, /*device_id=*/0);

struct hf3fs_ior ior;
hf3fs_iorcreate4(&ior, "/mnt/3fs", 64, true, 32, 5000, /*host numa=*/0, 0);

/* --- per-request: load a KV cache shard and D2D into engine memory --- */

void reload_kv_cache(int layer, void *engine_kv_ptr, size_t shard_bytes) {
  /* Open the persisted KV cache shard for this layer. */
  char path[256];
  snprintf(path, sizeof(path), "/mnt/3fs/kv_cache/layer_%d.bin", layer);
  int fd = open(path, O_RDONLY);
  hf3fs_reg_fd(fd, 0);

  /* GDR read: storage → RDMA → GPU staging buffer (zero host bounce). */
  hf3fs_prep_io(&ior, &staging_iov, /*is_read=*/true,
                staging_iov.base, fd, /*file_off=*/0, shard_bytes, NULL);
  hf3fs_submit_ios(&ior);

  struct hf3fs_cqe cqe;
  hf3fs_wait_for_ios(&ior, &cqe, 1, 1, NULL);

  /* D2D copy: staging buffer → engine's paged attention KV pool.
   * Stays entirely on-device — no CPU or PCIe hop. */
  cudaMemcpy(engine_kv_ptr, staging_iov.base, shard_bytes, cudaMemcpyDeviceToDevice);

  hf3fs_dereg_fd(fd);
  close(fd);
  /* staging_iov stays alive for the next request. */
}

/* --- shutdown --- */

hf3fs_iordestroy(&ior);
hf3fs_iovdestroy(&staging_iov);  /* frees the staging cudaMalloc */
```

### B. PyTorch integration (wrap an existing tensor)

```c
/* PyTorch allocated this GPU memory. Requires HF3FS_GDR_ENABLED build. */
void *tensor_ptr = /* t.data_ptr() */;
size_t tensor_size = /* t.nbytes() */;
uint8_t tensor_uuid[16] = { /* caller-generated UUID bytes */ };

struct hf3fs_iov iov;
hf3fs_iovwrap_device(&iov,
                     tensor_ptr,
                     tensor_uuid,
                     "/mnt/3fs",
                     tensor_size,
                     0,
                     /*device_id=*/0);

int fd = open("/mnt/3fs/data/batch.bin", O_RDONLY);
hf3fs_reg_fd(fd, 0);

struct hf3fs_ior ior;
hf3fs_iorcreate4(&ior, "/mnt/3fs", 1, true, 0, 5000, /*host numa=*/0, 0);

hf3fs_prep_io(&ior, &iov, true, tensor_ptr, fd, 0, tensor_size, NULL);
hf3fs_submit_ios(&ior);

struct hf3fs_cqe cqe;
hf3fs_wait_for_ios(&ior, &cqe, 1, 1, NULL);

/* Optional conservative fence before the next dependent CUDA kernel. */
hf3fs_iovsync(&iov, 1);

hf3fs_iordestroy(&ior);
hf3fs_iovdestroy(&iov);  /* Releases 3FS metadata/MRs only; PyTorch still owns tensor_ptr. */
hf3fs_dereg_fd(fd);
close(fd);
```

### C. Cross-process GPU sharing

```c
/* Process A: allocate and publish GPU memory. */
static const size_t kBytes = 1ULL << 30;
struct hf3fs_iov iov_a;
hf3fs_iovcreate_device(&iov_a, "/mnt/3fs", kBytes, 0, /*device_id=*/0);

uint8_t shared_id[16];
memcpy(shared_id, iov_a.id, sizeof(shared_id));
/* Pass shared_id to Process B via any control channel. */

/* Process B: open the same published GPU buffer (requires HF3FS_GDR_ENABLED). */
struct hf3fs_iov iov_b;
hf3fs_iovopen_device(&iov_b, shared_id, "/mnt/3fs", kBytes, 0, /*device_id=*/0);

/* Process B now has its own CUDA IPC import of the same underlying VRAM. */

hf3fs_iovdestroy(&iov_b);  /* closes Process B's imported mapping */
hf3fs_iovdestroy(&iov_a);  /* Process A must destroy last, because it owns the VRAM */
```

This cross-process case assumes both processes resolve GPU device 0 the same way. In containerized setups, device ordinal remapping must be consistent between the application and the fuse daemon.

---

## 7. Alternative Considerations & Known Limitations

1. **`nvidia_peermem` chosen over `dmabuf`.** `dmabuf` (`ibv_reg_dmabuf_mr` + CUDA VMM) requires VMM-based allocation, fd passing, kernel 5.12+, and RDMA-core ≥ 34. `nvidia_peermem` works with standard `cudaMalloc` and existing stacks. Note that `nvidia_peermem` is a runtime dependency — `hf3fs_gdr_available()` is a coarse capability check, not a per-buffer guarantee; actual MR registration failures surface during `ibv_reg_mr`. `dmabuf` may be revisited for vendor-neutral GPU support (AMD ROCm, Intel Level Zero).
2. **Explicit `_device` API with soft-fail fallback.** Separate `_device` entry points (instead of overloading `numa`) keep the API type-safe and unambiguous. `iovcreate_device` soft-fails to host allocation when GDR is unavailable, so applications need no per-call capability checks. Cross-process support is included from v1 because the fuse daemon is always a separate process in production.
3. **Cross-process lifetime is best-effort.** There is no distributed refcount or lease mechanism — exporter teardown must be coordinated by the caller. GPU namespace entries and importer-side state do not yet have the same orphan-cleanup coverage as host shm paths.
4. **Synchronization is conservative, not stream-aware.** `hf3fs_iovsync()` maps to device-wide `cudaDeviceSynchronize()` and does not integrate with CUDA events or streams. GPU `block_size` partitioning is also incomplete — the current path uses whole-buffer granularity.
5. **GPU↔NIC affinity uses sysfs-based topology.** PCIe BDF and NUMA node scoring covers common multi-GPU/multi-NIC topologies. Future refinement: integrate NVML `nvmlDeviceGetTopologyCommonAncestor` for finer-grained PCIe switch distance.
