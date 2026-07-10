#include <cerrno>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <type_traits>
#include <variant>

#include "fuse/IovTable.h"
#include "fuse/IovTypes.h"
#include "lib/api/hf3fs_usrbio.h"
#include "lib/common/CudaIpcMemory.h"
#include "tests/GtestHelpers.h"

#ifndef HF3FS_GDR_ENABLED

namespace hf3fs {
namespace {

using CreateDeviceFn = int (*)(hf3fs_iov *, const char *, size_t, size_t, int);

static_assert(std::is_same_v<decltype(&hf3fs_iovcreate_device), CreateDeviceFn>);
static_assert(std::is_same_v<fuse::IoBufForIO, lib::ShmBufForIO>);
static_assert(std::is_same_v<fuse::IovBuffer, std::variant<std::shared_ptr<lib::ShmBuf>>>);

TEST(GdrOffCompileGuard, CudaIpcStubsAndHostIovTypesRemainUsable) {
  EXPECT_FALSE(hf3fs_gdr_available());
  EXPECT_EQ(hf3fs_gdr_device_count(), 0);

  hf3fs_iov iov{};
  EXPECT_EQ(hf3fs_iovcreate_device(&iov, "/unused", 4096, 1, 0), -EINVAL);

  auto count = lib::cudaIpcDeviceCount();
  ASSERT_OK(count);
  EXPECT_EQ(*count, 0);

  auto available = lib::cudaIpcDeviceAvailable(0);
  ASSERT_OK(available);
  EXPECT_FALSE(*available);

  auto exported = lib::exportCudaIpcMemory(reinterpret_cast<void *>(uintptr_t{0x1000}), 4096, 0);
  ASSERT_ERROR(exported, StatusCode::kNotImplemented);

  fuse::IovEntry entry;
  EXPECT_FALSE(entry.isGpu());

  EXPECT_EQ(hf3fs_iovcreate_device(&iov, "/nonexistent", 4096, 0, 0), -ENOTSUP);
}

}  // namespace
}  // namespace hf3fs

#endif
