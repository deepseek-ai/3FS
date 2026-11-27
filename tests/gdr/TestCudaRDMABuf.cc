#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cuda.h>
#include <cuda_runtime.h>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "common/cuda/CudaMemory.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/RDMABuf.h"
#include "lib/common/CudaIpcMemory.h"
#include "lib/common/GpuShm.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net {
namespace {

std::optional<int> firstCudaDevice() {
  auto count = cuda::deviceCount();
  if (!count) {
    return std::nullopt;
  }
  for (int deviceId = 0; deviceId < *count; ++deviceId) {
    auto available = cuda::supportsIpc(deviceId);
    if (available && *available) {
      return deviceId;
    }
  }
  return std::nullopt;
}

uint8_t *deviceAddress(void *base, size_t offset) {
  return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(base) + offset);
}

bool ensureIbStarted() {
  if (IBManager::initialized()) {
    return !IBDevice::all().empty();
  }
  static IBConfig config;
  auto result = IBManager::start(config);
  return result && !IBDevice::all().empty();
}

std::string currentExecutable() {
  std::array<char, PATH_MAX> path{};
  auto length = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0) {
    return {};
  }
  path[length] = '\0';
  return path.data();
}

std::string toHex(const lib::CudaIpcHandle &handle) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.resize(handle.size() * 2);
  for (size_t i = 0; i < handle.size(); ++i) {
    result[i * 2] = digits[handle[i] >> 4];
    result[i * 2 + 1] = digits[handle[i] & 0x0f];
  }
  return result;
}

bool fromHex(std::string_view text, lib::CudaIpcHandle &handle) {
  if (text.size() != handle.size() * 2) {
    return false;
  }
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < handle.size(); ++i) {
    auto high = nibble(text[i * 2]);
    auto low = nibble(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    handle[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

int runIpcMrImporter(const lib::CudaIpcExport &exported, size_t viewSize, int deviceId) {
  auto executable = currentExecutable();
  if (executable.empty()) {
    return -1;
  }

  auto handle = toHex(exported.ipcHandle);
  auto allocationSize = std::to_string(exported.allocationSize);
  auto offset = std::to_string(exported.offset);
  auto size = std::to_string(viewSize);
  auto device = std::to_string(deviceId);
  setenv("HF3FS_GDR_PROBE_HANDLE", handle.c_str(), 1);
  setenv("HF3FS_GDR_PROBE_ALLOCATION_SIZE", allocationSize.c_str(), 1);
  setenv("HF3FS_GDR_PROBE_OFFSET", offset.c_str(), 1);
  setenv("HF3FS_GDR_PROBE_SIZE", size.c_str(), 1);
  setenv("HF3FS_GDR_PROBE_DEVICE", device.c_str(), 1);
  SCOPE_EXIT {
    unsetenv("HF3FS_GDR_PROBE_HANDLE");
    unsetenv("HF3FS_GDR_PROBE_ALLOCATION_SIZE");
    unsetenv("HF3FS_GDR_PROBE_OFFSET");
    unsetenv("HF3FS_GDR_PROBE_SIZE");
    unsetenv("HF3FS_GDR_PROBE_DEVICE");
  };

  auto child = fork();
  if (child == 0) {
    execl(executable.c_str(),
          executable.c_str(),
          "--gtest_filter=TestCudaRDMABuf.IpcMrImporterProcess",
          "--gtest_color=no",
          static_cast<char *>(nullptr));
    _exit(127);
  }
  if (child < 0) {
    return -1;
  }

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

TEST(TestCudaMemory, RejectsInvalidDeviceViews) {
  ASSERT_ERROR(cuda::inspectDeviceMemory(nullptr, 4096, 0), StatusCode::kInvalidArg);
  ASSERT_ERROR(cuda::prepareGdrMemory(reinterpret_cast<void *>(0x1000), 0, 0), StatusCode::kInvalidArg);
}

TEST(TestCudaMemory, PreparesWholeAllocationForGdr) {
  auto deviceId = firstCudaDevice();
  if (!deviceId) {
    GTEST_SKIP() << "No CUDA device available";
  }

  auto guard = cuda::ScopedDevice::create(*deviceId);
  ASSERT_OK(guard);
  void *allocation = nullptr;
  ASSERT_EQ(cudaMalloc(&allocation, 4096), cudaSuccess);
  SCOPE_EXIT { EXPECT_EQ(cudaFree(allocation), cudaSuccess); };

  auto *viewPtr = deviceAddress(allocation, 512);
  auto view = cuda::prepareGdrMemory(viewPtr, 1024, *deviceId);
  ASSERT_OK(view);
  EXPECT_EQ(view->allocationBase, allocation);
  EXPECT_GE(view->allocationSize, 4096u);
  EXPECT_EQ(view->offset, 512u);
  EXPECT_EQ(view->deviceId, *deviceId);

  unsigned int syncMemops = 0;
  ASSERT_EQ(
      cuPointerGetAttribute(&syncMemops, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, reinterpret_cast<CUdeviceptr>(viewPtr)),
      CUDA_SUCCESS);
  EXPECT_EQ(syncMemops, 1u);
}

TEST(TestCudaRDMABuf, RegistersEveryActiveHcaAndSharesOwnerAcrossSubranges) {
  auto deviceId = firstCudaDevice();
  if (!deviceId) {
    GTEST_SKIP() << "No CUDA device available";
  }
  if (!ensureIbStarted()) {
    GTEST_SKIP() << "No active IB device available";
  }

  auto guard = cuda::ScopedDevice::create(*deviceId);
  ASSERT_OK(guard);
  void *allocation = nullptr;
  ASSERT_EQ(cudaMalloc(&allocation, 4096), cudaSuccess);
  SCOPE_EXIT { EXPECT_EQ(cudaFree(allocation), cudaSuccess); };

  std::atomic<bool> ownerReleased = false;
  auto owner = std::shared_ptr<void>(reinterpret_cast<void *>(1), [&](void *) { ownerReleased.store(true); });
  auto rdmaBuf = RDMABuf::createFromCudaBuffer(static_cast<uint8_t *>(allocation), 4096, *deviceId, owner);
  ASSERT_OK(rdmaBuf);
  owner.reset();

  EXPECT_TRUE(rdmaBuf->isDeviceMemory());
  EXPECT_EQ(rdmaBuf->cudaDeviceId(), *deviceId);
  auto remote = rdmaBuf->toRemoteBuf();
  ASSERT_TRUE(remote);
  for (const auto &device : IBDevice::all()) {
    EXPECT_NE(rdmaBuf->getMR(device->id()), nullptr);
    EXPECT_TRUE(remote.getRkey(device->id()).has_value());
  }

  auto subrange = rdmaBuf->subrange(128, 512);
  ASSERT_TRUE(subrange);
  EXPECT_TRUE(subrange.isDeviceMemory());
  EXPECT_EQ(subrange.cudaDeviceId(), *deviceId);
  *rdmaBuf = RDMABuf();
  EXPECT_FALSE(ownerReleased.load());
  subrange = RDMABuf();
  EXPECT_TRUE(ownerReleased.load());
}

TEST(TestCudaRDMABuf, IpcMrImporterProcess) {
  auto handleText = std::getenv("HF3FS_GDR_PROBE_HANDLE");
  if (!handleText) {
    GTEST_SKIP() << "Subprocess-only CUDA IPC MR probe";
  }
  ASSERT_TRUE(ensureIbStarted());

  auto allocationSizeText = std::getenv("HF3FS_GDR_PROBE_ALLOCATION_SIZE");
  auto offsetText = std::getenv("HF3FS_GDR_PROBE_OFFSET");
  auto sizeText = std::getenv("HF3FS_GDR_PROBE_SIZE");
  auto deviceText = std::getenv("HF3FS_GDR_PROBE_DEVICE");
  ASSERT_NE(allocationSizeText, nullptr);
  ASSERT_NE(offsetText, nullptr);
  ASSERT_NE(sizeText, nullptr);
  ASSERT_NE(deviceText, nullptr);

  lib::CudaIpcHandle handle{};
  ASSERT_TRUE(fromHex(handleText, handle));
  auto allocationSize = std::strtoull(allocationSizeText, nullptr, 10);
  auto offset = std::strtoull(offsetText, nullptr, 10);
  auto size = std::strtoull(sizeText, nullptr, 10);
  auto deviceId = std::atoi(deviceText);

  auto guard = cuda::ScopedDevice::create(deviceId);
  ASSERT_OK(guard);
  auto gpuShm = lib::GpuShmBuf::create(handle, allocationSize, offset, size, deviceId);
  ASSERT_OK(gpuShm);
  auto *ioBuffer = (*gpuShm)->ioBuffer();
  ASSERT_NE(ioBuffer, nullptr);
  EXPECT_TRUE(ioBuffer->isDeviceMemory());

  unsigned int syncMemops = 0;
  ASSERT_EQ(cuPointerGetAttribute(&syncMemops,
                                  CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                  reinterpret_cast<CUdeviceptr>(ioBuffer->data())),
            CUDA_SUCCESS);
  EXPECT_EQ(syncMemops, 1u);

  auto remote = ioBuffer->subrangeRemote(0, ioBuffer->size());
  ASSERT_TRUE(remote);
  for (const auto &device : IBDevice::all()) {
    EXPECT_TRUE(remote.getRkey(device->id()).has_value());
  }
}

TEST(TestCudaRDMABuf, IpcImportedPointerRegistersWithEveryHca) {
  auto deviceId = firstCudaDevice();
  if (!deviceId) {
    GTEST_SKIP() << "No CUDA device available";
  }
  if (!ensureIbStarted()) {
    GTEST_SKIP() << "No active IB device available";
  }
  if (currentExecutable().empty()) {
    GTEST_SKIP() << "/proc/self/exe is unavailable";
  }

  auto guard = cuda::ScopedDevice::create(*deviceId);
  ASSERT_OK(guard);
  void *allocation = nullptr;
  ASSERT_EQ(cudaMalloc(&allocation, 4096), cudaSuccess);
  SCOPE_EXIT { EXPECT_EQ(cudaFree(allocation), cudaSuccess); };

  auto *view = deviceAddress(allocation, 256);
  auto exported = lib::exportCudaIpcMemory(view, 1024, *deviceId);
  ASSERT_OK(exported);
  EXPECT_EQ(runIpcMrImporter(*exported, 1024, *deviceId), 0);
}

}  // namespace hf3fs::net
