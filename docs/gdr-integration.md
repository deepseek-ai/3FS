# GPU Direct RDMA (GDR) Integration Guide

## Overview

GPU Direct RDMA (GDR) lets the 3FS FUSE data path transfer data directly
between RDMA and CUDA device memory. The application process does not register
GPU memory with InfiniBand. It allocates or wraps CUDA memory, exports a CUDA
IPC capability, and publishes that capability through the 3FS virtual IOV
namespace. The FUSE process imports the CUDA allocation and owns the
`ibv_mr` objects for the published view.

This split creates two distinct capabilities:

1. **Application-side CUDA IPC capability.** `hf3fs_gdr_available()` reports
   whether the local process can use CUDA IPC on at least one visible device.
   It does not inspect FUSE, `nvidia_peermem`, the selected NIC, or
   `ibv_reg_mr`.
2. **FUSE-side GDR capability.** When FUSE resolves a GDR IOV publication, it
   imports the CUDA IPC allocation and registers the requested view with every
   active IB device.

A successful application capability query is therefore only a prerequisite.
The return value from the publication operation (`hf3fs_iovcreate_device()` or
`hf3fs_iovwrap_device()`) is the final per-buffer result: success means FUSE
accepted the symlink, imported the CUDA allocation, and registered the
published view with every active IB device. `ibv_reg_mr` can still fail after
`hf3fs_gdr_available()` returned `true`.

## Prerequisites

### Hardware

- A CUDA device that supports unified addressing and is not in prohibited
  compute mode.
- An InfiniBand or RoCE device that can register the CUDA allocation.
- A GPU/NIC/driver combination that supports peer-memory registration.

PCIe locality affects performance. It does not replace the final registration
check.

### Software

- A working CUDA runtime and driver.
- RDMA userspace and kernel drivers.
- NVIDIA peer-memory support, normally provided by `nvidia_peermem`:

  ```bash
  sudo modprobe nvidia_peermem
  lsmod | grep nvidia_peermem
  ```

### Build

Build 3FS with GDR support:

```bash
cmake -DHF3FS_ENABLE_GDR=ON ...
```

- `HF3FS_ENABLE_GDR` requests CUDA/GDR support at CMake configure time.
- The same name is defined as `HF3FS_ENABLE_GDR=1` for GDR-enabled C++ targets.
- Configuration fails when GDR is requested but the CUDA runtime or driver
  library is unavailable. There is no separate runtime environment switch.

`hf3fs_iovopen_device()` and `hf3fs_iovwrap_device()` are declared only when
the consuming target is compiled with `HF3FS_ENABLE_GDR`.
`hf3fs_iovcreate_device()` remains declared in all builds for API
compatibility, but returns `-ENOTSUP` when CUDA/GDR support is not compiled in.

### Runtime

- Start `IBManager` before publishing a GPU IOV. Per-buffer registration fails
  if IB has not started or no active IB device exists.
- Run the application on the same host as the FUSE process that serves the
  mount.
- The application and FUSE process must be allowed to share CUDA allocations
  through CUDA IPC.
- The FUSE virtual IOV namespace must accept the `.gdr.d{device_id}`
  publication. Publication failure is returned to the application as
  `-errno`.

### No Host Fallback

The device APIs never return a host IOV. A build without CUDA/GDR support, no
locally usable CUDA IPC device, an invalid device pointer, CUDA IPC failure,
FUSE import failure, or RDMA registration failure is returned as an error.
A CUDA device pointer is never passed to the host-memory registration path.
Applications that want host memory must call `hf3fs_iovcreate()` explicitly.

## API Reference

All functions return `0` on success and a negative errno value on failure
unless stated otherwise. The caller owns the `struct hf3fs_iov` object itself;
the library fills its fields.

Include:

```c
#include <hf3fs_usrbio.h>
```

### Device IOV Creation

#### `hf3fs_iovcreate_device`

Allocate CUDA memory, export the whole allocation through CUDA IPC, and
publish a view covering the allocation:

```c
int hf3fs_iovcreate_device(struct hf3fs_iov *iov,
                           const char *hf3fs_mount_point,
                           size_t size,
                           size_t block_size,
                           int device_id);
```

Constraints:

- `iov` and `hf3fs_mount_point` must be non-null.
- `size` must be non-zero.
- `block_size` must be `0`; GDR IOV block partitioning is unsupported.
- `device_id` must name a locally usable CUDA IPC device.

On the CUDA path, the application owns the allocation and the publication.
The application does **not** create or own an RDMA MR. Publication success is
the final FUSE-side import/registration result. On destroy, the publisher
unlinks the publication and frees its CUDA allocation.

Representative errors include `-EINVAL` for invalid arguments,
`-ENOTSUP` when CUDA/GDR support or local CUDA IPC capability is unavailable,
`-ENODEV` for an unavailable selected device, `-ENOMEM` for allocation
failure, `-EIO` for CUDA IPC or FUSE registration failures, and the exact
negative errno returned by publication (for example, `-EEXIST` for a
conflicting key). No failure is converted into a host allocation.

---

#### `hf3fs_iovopen_device`

Open an existing device IOV by UUID:

```c
int hf3fs_iovopen_device(struct hf3fs_iov *iov,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);
```

This API reads the existing GDR v2 publication, imports its CUDA allocation,
and exposes the published view in the importing application. It does not
create a second publication and does not register an application-side MR.

Constraints:

- The original publisher and its CUDA allocation must still be alive.
- `size` and `device_id` must match the publication.
- `block_size` must be `0`.
- The imported handle must resolve to an allocation base, and its actual
  allocation size must match the v2 `allocation` field.

The importer borrows the publication. Its `hf3fs_iovunlink()` is a no-op for
that GPU IOV, and its `hf3fs_iovdestroy()` closes only its CUDA IPC mapping and
local metadata.

Representative errors include `-ENOTSUP` when local CUDA IPC capability is
unavailable, `-ENOENT` when the publication does not exist, `-ENODEV` for an
unavailable device, `-EINVAL` for malformed or mismatched metadata, and
`-EIO` for CUDA import failures.

**Availability:** declared only with `HF3FS_ENABLE_GDR`.

---

#### `hf3fs_iovwrap_device`

Publish a view of an existing CUDA allocation:

```c
int hf3fs_iovwrap_device(struct hf3fs_iov *iov,
                         void *device_ptr,
                         const uint8_t id[16],
                         const char *hf3fs_mount_point,
                         size_t size,
                         size_t block_size,
                         int device_id);
```

`device_ptr` may be the allocation base or a pointer to a view/suballocation,
including a PyTorch tensor view. The implementation uses
`cuMemGetAddressRange()` to discover the underlying allocation base, full
allocation size, and view offset. It verifies that
`[device_ptr, device_ptr + size)` is within that allocation and exports the
allocation base through CUDA IPC.

The caller retains ownership of the CUDA allocation and must keep the entire
underlying allocation alive until the publication and all importers are gone.
The wrapped IOV owns the publication but not the allocation.
`hf3fs_iovdestroy()` unlinks the publication and releases 3FS metadata; it does
not call `cudaFree()` on the wrapped allocation.

Constraints:

- `device_ptr`, `id`, and `hf3fs_mount_point` must be non-null.
- `size` must be non-zero and fit within the discovered allocation.
- `device_id` must match the CUDA pointer's device.
- `id` must be unique in the mount's IOV namespace.
- `block_size` must be `0`.

Representative errors include `-ENOTSUP`, `-ENODEV`, `-EINVAL`, `-ENOMEM`,
`-EIO`, and publication errors returned as negative errno values. The wrapped
allocation remains caller-owned on every failure path.

**Availability:** declared only with `HF3FS_ENABLE_GDR`.

### IOV Lifecycle

#### `hf3fs_iovdestroy`

```c
void hf3fs_iovdestroy(struct hf3fs_iov *iov);
```

The effect depends on how the IOV was obtained:

- `hf3fs_iovcreate_device()`: unlink the owned publication, then free the CUDA
  allocation.
- `hf3fs_iovwrap_device()`: unlink the owned publication, but leave the
  caller-owned CUDA allocation untouched.
- `hf3fs_iovopen_device()`: close the imported CUDA mapping without unlinking
  the borrowed publication.

For cross-process sharing, importers destroy first and the exporter destroys
last. There is no distributed reference count.

#### `hf3fs_iovunlink`

```c
void hf3fs_iovunlink(struct hf3fs_iov *iov);
```

For a create/wrap publisher, remove the GDR publication while leaving the
local IOV object alive. For an open importer, this is intentionally a no-op:
an importer is not allowed to remove the exporter's publication.

The publisher must wait for all I/O using the IOV to complete and destroy every
application importer before unlinking or destroying the publication. CUDA
requires the exported allocation to remain alive until every imported mapping
has been closed.

### Synchronization

#### `hf3fs_iovsync`

```c
int hf3fs_iovsync(const struct hf3fs_iov *iov, int direction);
```

For a GPU IOV, this selects its CUDA device and calls
`cudaDeviceSynchronize()`. The current implementation accepts the direction
convention but uses the same device-wide synchronization for both values:

- `direction = 0`: application GPU writes must be visible before submitting a
  storage write.
- `direction = 1`: RDMA writes must be visible before the application consumes
  data on the GPU.

The function is a no-op for host IOVs. It returns `-EINVAL` for an invalid GPU
IOV, `-ENODEV` if its device cannot be selected, and `-EIO` if synchronization
fails.

### Capability Queries

#### `hf3fs_gdr_available`

```c
bool hf3fs_gdr_available(void);
```

Returns `true` when at least one locally visible CUDA device supports the
application's CUDA IPC requirements (unified addressing and a usable compute
mode). It does not query FUSE or prove that an MR can be created.

Use the return value of create/wrap publication, which includes FUSE-side
import and registration, as the final per-buffer decision.

#### `hf3fs_gdr_device_count`

```c
int hf3fs_gdr_device_count(void);
```

Returns the local CUDA device count, or `0` when CUDA enumeration is
unavailable. It does not query FUSE or probe RDMA registration.

### Additional Utilities

#### `hf3fs_iov_mem_type`

```c
enum hf3fs_mem_type hf3fs_iov_mem_type(const struct hf3fs_iov *iov);
```

Returns `HF3FS_MEM_DEVICE` for a tracked GPU IOV and `HF3FS_MEM_HOST`
otherwise. The enum also reserves `HF3FS_MEM_MANAGED`, but these device APIs
do not report that value.

#### `hf3fs_iov_device_id`

```c
int hf3fs_iov_device_id(const struct hf3fs_iov *iov);
```

Returns the CUDA device index for a tracked GPU IOV, or `-1` for a host or
invalid IOV.

### I/O Submission

The normal user I/O APIs accept a GPU IOV:

- `hf3fs_prep_io()` uses `ptr` as an address within the IOV view.
- `hf3fs_submit_ios()` submits prepared operations.
- `hf3fs_wait_for_ios()` returns completion entries.

For a GPU IOV, `iov->base` and pointers derived from it are CUDA device
pointers. The application passes them as opaque addresses; it must not
dereference them on the CPU.

## Usage Examples

### Example A: Require a GDR Allocation

`hf3fs_gdr_available()` is useful for an early local check, but the create
result is authoritative:

```c
struct hf3fs_iov iov = {0};

if (!hf3fs_gdr_available()) {
    /* No local CUDA IPC capability; do not request mandatory GDR. */
    return -ENOTSUP;
}

int rc = hf3fs_iovcreate_device(&iov, "/mnt/3fs",
                                128UL * 1024 * 1024,
                                /*block_size=*/0,
                                /*device_id=*/0);
if (rc != 0) {
    /* Includes FUSE CUDA import or final MR registration failure. */
    return rc;
}

/* Prepare and complete I/O using addresses within iov.base. */

rc = hf3fs_iovsync(&iov, /*direction=*/1);
hf3fs_iovdestroy(&iov);
return rc;
```

No application-side `ibv_reg_mr()` call is required or owned by this code.

### Example B: Publish a PyTorch View

The wrapped pointer may be inside a larger allocator-owned CUDA allocation:

```c
/* tensor_ptr and tensor_bytes come from a live CUDA PyTorch tensor/view.
 * uuid is a unique 16-byte identifier supplied by the application.
 */
struct hf3fs_iov iov = {0};
int rc = hf3fs_iovwrap_device(&iov,
                              tensor_ptr,
                              uuid,
                              "/mnt/3fs",
                              tensor_bytes,
                              /*block_size=*/0,
                              tensor_device);
if (rc != 0) {
    /* The tensor remains caller-owned on every failure path. */
    return rc;
}

/* Use iov.base only as a CUDA/RDMA address. */

hf3fs_iovdestroy(&iov);  /* Removes the publication; does not free tensor_ptr. */
```

The whole underlying CUDA allocation remains the CUDA IPC sharing unit even
when only a tensor view is published. The application must keep that
allocation alive until all FUSE and application importers have released it.

### Example C: Cross-Process Open

The exporter publishes and shares `iov.id` through an application-defined
control channel. An importer borrows that publication:

```c
struct hf3fs_iov imported = {0};
int rc = hf3fs_iovopen_device(&imported,
                              exported_uuid,
                              "/mnt/3fs",
                              exported_view_size,
                              /*block_size=*/0,
                              exported_device);
if (rc != 0) {
    return rc;
}

/* Submit I/O against imported.base. */

hf3fs_iovdestroy(&imported);  /* Closes this mapping; does not unlink. */
```

Required shutdown order:

1. Stop and reap I/O that uses the imported view.
2. Destroy every application importer.
3. Destroy the create/wrap exporter last.

The publisher owns the publication. Importer-first cleanup cannot unlink it.

## Runtime Behavior

### Capability and Ownership Model

The application process owns CUDA allocation/IPC publication only:

- create owns both the allocation and publication;
- wrap owns the publication while the caller owns the allocation;
- open owns an imported CUDA mapping and borrows the publication.

FUSE owns the data-plane import and registration:

- the v2 URI is parsed and the full CUDA allocation is imported;
- only the published view is exposed to I/O and offered for RDMA registration;
- one shared `RDMABuf::Inner` owns every per-IB `ibv_mr`, the CUDA device
  identity, and the imported mapping owner;
- publication succeeds only when every active IB device accepts the MR.

Removing a FUSE GPU IOV releases its `IOBuffer`. `RDMABuf::Inner` first
deregisters every MR and only then releases the owner that closes the CUDA IPC
mapping. FUSE shutdown clears GPU IOVs before the process-wide shutdown stops
`IBManager`. The lifetime constraints are:

```text
IBManager outlives every ibv_mr
CUDA IPC mapping outlives every ibv_mr that covers it
exporter allocation outlives every CUDA IPC mapping
```

Explicit unlink and shutdown paths preserve these constraints. An exporter
that exits or frees its allocation before importers close violates the CUDA IPC
contract; 3FS cannot repair that ordering after the fact.

### GDR URI v2 Contract

The GDR symlink key and target are:

```text
{uuid}.gdr.d{device_id} ->
gdr://v2/device/{device}/allocation/{allocation_bytes}/offset/{view_offset}/size/{view_bytes}/ipc/{128_hex_chars}
```

Fields:

- `device`: non-negative CUDA device index.
- `allocation`: byte size of the complete CUDA allocation represented by the
  IPC handle.
- `offset`: byte offset of the published view from the allocation base.
- `size`: byte size of the published view and IOV.
- `ipc`: exactly 64 bytes of CUDA IPC handle encoded as 128 hexadecimal
  characters.

The parser is strict:

- the `gdr://v2` prefix, field names, separators, and field order are exact;
- numeric fields contain ASCII decimal digits only;
- `device` must fit in `int`;
- `allocation`, `offset`, and `size` must fit in `size_t`;
- `allocation` and `size` must be non-zero;
- `offset <= allocation` and `size <= allocation - offset`;
- `ipc` must contain exactly 128 valid hexadecimal characters (upper- or
  lower-case is accepted; the formatter emits lower-case);
- trailing or missing data is rejected.

FUSE also requires the device in the key to equal the URI device. GDR keys
reject block-size and I/O-ring attributes. `hf3fs_iovopen_device()` additionally
requires its requested device and view size to match, and CUDA import verifies
that the imported pointer is the allocation base and that the actual allocation
size matches the URI.

### CUDA IPC Trust Boundary

CUDA IPC shares the complete underlying allocation, not an independently
protected subrange. `offset` and `size` constrain the 3FS IOV and the RDMA MR,
but the importing FUSE process maps the full allocation before deriving that
view.

Treat FUSE and any application importer as trusted with respect to the full
allocation. Do not co-locate unrelated secrets in the same CUDA allocation
when that trust is unacceptable. A PyTorch view remains valid only while its
underlying allocator-owned block is alive and has not been reused.

### Tagged IOV Table and Ring Ordering

FUSE stores host and GPU buffers as tagged alternatives in one `IovEntry`
table. Namespace-key and UUID I/O lookup resolve the same entry, and its memory
kind selects the host or GPU view. This keeps access checks, descriptor
identity, and lifetime ownership in one table.

An I/O-ring batch has one absolute logical order even when its storage wraps
at the physical end of the ring. File lookup, buffer lookup, submission, and
completion preserve that order across both physical spans.

### Device Memory Semantics

A CUDA device pointer is not CPU-dereferenceable. Do not use `memcpy`,
`memset`, CPU checksum routines, string/formatting reads, or ordinary C/C++
loads and stores on `iov->base`. Use CUDA APIs or kernels, or pass the pointer
as an opaque I/O address.

Before an allocation is exported or directly registered, 3FS validates its
CUDA device and allocation bounds and enables
`CU_POINTER_ATTRIBUTE_SYNC_MEMOPS` for the whole allocation. FUSE repeats this
preparation on its imported CUDA mapping before `ibv_reg_mr`; owner-side and
importer-side CUDA contexts must both use conservative memory-operation
semantics. Address offsets inside 3FS use checked integer arithmetic;
converting a GPU-backed `RDMABuf` to a host `span` or `ByteRange` is a fatal
invariant violation.

For GPU I/O:

- inline reads are disabled for a batch containing a GPU buffer;
- inline writes are disabled for GPU buffers;
- client-side CPU checksum calculation/comparison is disabled because the CPU
  cannot read the buffer;
- failed GPU MR creation is an error; the host fallback path has been removed.

Skipping the client CPU comparison does not provide independent end-to-end
verification. Applications that require that property must validate the data
with a GPU-capable mechanism.

### Sparse Read Holes

Sparse-read zero filling is memory-kind aware. Host ranges use `memset`; CUDA
device ranges use `cudaMemset` on the owning device. Device zeroing is followed
by `cudaDeviceSynchronize()` before the I/O result is finalized and before the
CQE is published. A CUDA zeroing or synchronization error becomes the I/O
error instead of exposing a completion for unfinished zero fill.

`HF3FS_IOR_FORBID_READ_HOLES` continues to reject holes instead of filling
them.

### GPU Write Checksums

`ChecksumType::NONE` means checksum verification is disabled. It is not an
implicit request for storage-side checksum computation. This remains true when
the per-request verification option is enabled: a configured target type of
`NONE` sends a `NONE` checksum and does not set
`FeatureFlags::SERVER_COMPUTE_CHECKSUM`.

For a GPU write with checksum verification enabled:

1. The client sends the configured non-`NONE` checksum type with a placeholder
   value and sets `FeatureFlags::SERVER_COMPUTE_CHECKSUM`.
2. Storage completes the RDMA read into its host staging buffer.
3. Storage computes the requested checksum over that host staging data and
   replaces the placeholder before dispatching the update to the backend.

Storage rejects the flag for a non-WRITE update, a `NONE` target type, missing
staged data, or an RDMA-bypass request that supplies neither RDMA nor inline
data.

There is no capability negotiation for this feature. During a rolling upgrade,
upgrade every storage node before enabling GPU writes from new clients. A new
client can send a placeholder checksum to an old storage node; the old node
does not materialize it and can report a checksum mismatch. Do not enable this
path while an old storage node can be the first node handling the write.

Backend behavior remains backend-specific:

- The default `ChunkReplica` path never clears the `ChunkMetadata` object merely
  because verification is disabled. A server-computed non-`NONE` checksum
  follows the existing verification and metadata-update path. With request
  type `NONE`, only the checksum fields become the disabled state (`NONE`,
  value `0`); version, state, size, identity, and other metadata continue
  through the normal update path.
- `ChunkEngine` uses its `without_checksum` request semantic when verification
  is disabled, but continues to maintain and return its CRC32C chunk metadata
  for non-remove chunks. Thus request `NONE` does not mean that ChunkEngine
  lacks internal checksum metadata.

## Known Limitations

- **Per-buffer MR registration is authoritative.** Local CUDA IPC capability
  does not guarantee that a specific GPU/view/NIC registration will succeed.
- **`nvidia_peermem` is required by the implemented registration path.**
  Device API failures are returned to the caller; no host IOV is substituted.
- **CUDA IPC exposes the whole allocation.** View bounds restrict 3FS and RDMA,
  not the CUDA IPC mapping's trust boundary.
- **Device-wide synchronization.** `hf3fs_iovsync()` and sparse device zeroing
  use `cudaDeviceSynchronize()`, not stream-scoped fencing.
- **No GDR block partitioning.** Device IOV APIs require `block_size == 0`.
- **Publisher-coordinated lifetime.** There is no distributed reference count;
  all I/O must complete and the exporter must outlive FUSE use and every
  importer.
- **All-HCA registration.** Partial-HCA GDR is not supported. A GPU IOV is
  rejected if any active IB device cannot register it; transport selection is
  therefore never given a sparse rkey set.
- **IPC-import MR support is platform-dependent.** The target driver,
  `nvidia_peermem`, HCA, and CUDA combination must pass the two-process
  `IpcImportedPointerRegistersWithEveryHca` hardware test.
- **No registration cache.** An IOV is registered once for its published
  lifetime. Add a 64KB-normalized, allocation-ID-aware cache only if measured
  registration latency requires it.

## Target-Machine Validation

Run the GDR test binary on the NVIDIA/RDMA host used for deployment:

```bash
ctest --test-dir build -R test_gdr --output-on-failure
```

Do not accept a skipped
`TestCudaRDMABuf.IpcImportedPointerRegistersWithEveryHca` test as GDR
validation. That two-process test proves that the exporter can prepare and
publish a CUDA allocation, the importer can enable `SYNC_MEMOPS`, and every
active HCA can register the imported view. The deployment acceptance test must
also submit at least one real storage read and write through a mounted GDR IOV;
the unit test does not emulate an HCA data transfer.

## References

- [NVIDIA GPUDirect RDMA: synchronization, registration, and memory ordering](https://docs.nvidia.com/cuda/gpudirect-rdma/)
- [NVIDIA CUDA Driver API: pointer attributes and `SYNC_MEMOPS`](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__UNIFIED.html)
