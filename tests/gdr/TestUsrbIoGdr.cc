/**
 * Scenario tests for Layer 3+4: UsrbIoGdr + UsrbIo (unified C API)
 *
 * Tests GPU IOV create/open/wrap/destroy, query functions, and sync.
 * Covers core dispatch paths and unsupported-device behavior.
 *
 * Covers: REQ-L3-001, REQ-L3-003, REQ-L3-005
 *         REQ-L4-001 through REQ-L4-006
 *         INV-GDR-001, INV-GDR-002
 */

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <limits.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HF3FS_GDR_ENABLED
#include <cuda_runtime.h>
#endif

#include "common/utils/Uuid.h"
#include "lib/api/UsrbIoGdrInternal.h"
#include "lib/api/hf3fs_usrbio.h"
#include "lib/common/CudaIpcMemory.h"
#include "lib/common/GdrUri.h"
#include "tests/GtestHelpers.h"

namespace {

static bool hasGpu() { return hf3fs_gdr_available(); }

// Temp directory for symlink testing
class TmpDir {
 public:
  TmpDir() {
    const char *base = getenv("TMPDIR");
    path_ = std::string(base ? base : std::filesystem::temp_directory_path().c_str()) + "/gdr_test_XXXXXX";
    char *result = mkdtemp(path_.data());
    if (result) {
      valid_ = true;
    }
  }

  ~TmpDir() {
    if (valid_) {
      std::filesystem::remove_all(path_);
    }
  }

  const char *path() const { return path_.c_str(); }
  bool valid() const { return valid_; }

 private:
  std::string path_;
  bool valid_ = false;
};

// Helper to build a well-formed GDR URI
std::string buildGdrUri(int deviceId, size_t allocationSize, size_t offset, size_t size, const uint8_t ipcHandle[64]) {
  char hex[129];
  for (int i = 0; i < 64; i++) {
    snprintf(hex + i * 2, 3, "%02x", ipcHandle[i]);
  }
  hex[128] = '\0';
  return std::string("gdr://v2/device/") + std::to_string(deviceId) + "/allocation/" + std::to_string(allocationSize) +
         "/offset/" + std::to_string(offset) + "/size/" + std::to_string(size) + "/ipc/" + hex;
}

hf3fs::Uuid iovUuid(const struct hf3fs_iov &iov) {
  hf3fs::Uuid id;
  std::memcpy(id.data, iov.id, sizeof(id.data));
  return id;
}

std::string publicationPath(const struct hf3fs_iov &iov, int deviceId) {
  return std::string(iov.mount_point) + "/3fs-virt/iovs/" + iovUuid(iov).toHexString() + ".gdr.d" +
         std::to_string(deviceId);
}

bool isSymlink(const std::string &path) {
  struct stat statbuf{};
  return lstat(path.c_str(), &statbuf) == 0 && S_ISLNK(statbuf.st_mode);
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

bool runImporterProcess(const struct hf3fs_iov &publisher, int deviceId, uint8_t expectedByte) {
  auto executable = currentExecutable();
  if (executable.empty()) {
    return false;
  }

  auto size = std::to_string(publisher.size);
  auto device = std::to_string(deviceId);
  auto expected = std::to_string(expectedByte);
  auto id = iovUuid(publisher).toHexString();
  setenv("HF3FS_GDR_IMPORT_MOUNT", publisher.mount_point, 1);
  setenv("HF3FS_GDR_IMPORT_ID", id.c_str(), 1);
  setenv("HF3FS_GDR_IMPORT_SIZE", size.c_str(), 1);
  setenv("HF3FS_GDR_IMPORT_DEVICE", device.c_str(), 1);
  setenv("HF3FS_GDR_IMPORT_EXPECTED_BYTE", expected.c_str(), 1);
  SCOPE_EXIT {
    unsetenv("HF3FS_GDR_IMPORT_MOUNT");
    unsetenv("HF3FS_GDR_IMPORT_ID");
    unsetenv("HF3FS_GDR_IMPORT_SIZE");
    unsetenv("HF3FS_GDR_IMPORT_DEVICE");
    unsetenv("HF3FS_GDR_IMPORT_EXPECTED_BYTE");
  };

  auto child = fork();
  if (child == 0) {
    execl(executable.c_str(),
          executable.c_str(),
          "--gtest_filter=TestUsrbIoGdrFixture.ImporterProcessKeepsPublisherPublication",
          "--gtest_color=no",
          static_cast<char *>(nullptr));
    _exit(127);
  }
  if (child < 0) {
    return false;
  }

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class TestUsrbIoGdrFixture : public ::testing::Test {};

}  // namespace

// ==========================================================================
// REQ-L4-001: iovcreate host path; iovcreate_device dispatches to GPU
// ==========================================================================

// @tests SCN-L4-001-01b
TEST_F(TestUsrbIoGdrFixture, SCN_L4_001_01b_DeviceApiDispatch) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));

  // WHEN: iovcreate_device with device 0
  int rc = hf3fs_iovcreate_device(&iov, "/nonexistent/mount", 4096, 0, 0);

  // THEN: Should fail (no mount) but exercise dispatch
  EXPECT_NE(rc, 0);
}

// @tests SCN-L4-001-02
TEST_F(TestUsrbIoGdrFixture, SCN_L4_001_02_DeviceCreateRejectsUnavailableGDR) {
  if (hasGpu()) {
    GTEST_SKIP() << "Test for machines without GPU";
  }

  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));

  // WHEN: iovcreate_device, GDR unavailable
  int rc = hf3fs_iovcreate_device(&iov, "/nonexistent/mount", 4096, 0, 0);

  // THEN: Device creation never silently substitutes host memory.
  EXPECT_EQ(rc, -ENOTSUP);
}

TEST_F(TestUsrbIoGdrFixture, DeviceApisRejectGpuBlockSize) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  uint8_t id[16] = {};
  uint8_t buffer[4096] = {};

  EXPECT_EQ(hf3fs_iovcreate_device(&iov, "/nonexistent/mount", 4096, 4096, 0), -EINVAL);
  EXPECT_EQ(hf3fs_iovopen_device(&iov, id, "/nonexistent/mount", 4096, 4096, 0), -EINVAL);
  EXPECT_EQ(hf3fs_iovwrap_device(&iov, buffer, id, "/nonexistent/mount", 4096, 4096, 0), -EINVAL);
}

TEST_F(TestUsrbIoGdrFixture, PublicationRemovalRespectsOwnershipWithoutCuda) {
  TmpDir tmpDir;
  ASSERT_TRUE(tmpDir.valid());
  std::filesystem::create_directories(std::string(tmpDir.path()) + "/3fs-virt/iovs");

  struct hf3fs_iov iov{};
  auto id = hf3fs::Uuid::random();
  std::memcpy(iov.id, id.data, sizeof(iov.id));
  std::strcpy(iov.mount_point, tmpDir.path());
  auto link = publicationPath(iov, 3);
  ASSERT_EQ(symlink("gdr://test-publication", link.c_str()), 0);
  ASSERT_TRUE(isSymlink(link));

  EXPECT_EQ(hf3fs_iovunlink_gpu_publication_internal(&iov, 3, false), 0);
  EXPECT_TRUE(isSymlink(link));

  EXPECT_EQ(hf3fs_iovunlink_gpu_publication_internal(&iov, 3, true), 0);
  EXPECT_FALSE(isSymlink(link));
  EXPECT_EQ(hf3fs_iovunlink_gpu_publication_internal(&iov, 3, true), -ENOENT);
}

TEST_F(TestUsrbIoGdrFixture, CudaIpcCapabilityAndSuballocationExportDoNotRequireIbRegistration) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No CUDA IPC-capable GPU available";
  }

  int deviceId = -1;
  ASSERT_EQ(cudaGetDevice(&deviceId), cudaSuccess);
  void *allocation = nullptr;
  ASSERT_EQ(cudaMalloc(&allocation, 4096), cudaSuccess);
  SCOPE_EXIT { EXPECT_EQ(cudaFree(allocation), cudaSuccess); };

  auto *view = static_cast<uint8_t *>(allocation) + 512;
  auto exported = hf3fs::lib::exportCudaIpcMemory(view, 1024, deviceId);
  ASSERT_OK(exported);
  EXPECT_EQ(exported->allocationBase, allocation);
  EXPECT_GE(exported->allocationSize, 1536u);
  EXPECT_EQ(exported->offset, 512u);
}

TEST_F(TestUsrbIoGdrFixture, CreateOwnsAndRemovesPublication) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No CUDA IPC-capable GPU available";
  }

  TmpDir tmpDir;
  ASSERT_TRUE(tmpDir.valid());
  std::filesystem::create_directories(std::string(tmpDir.path()) + "/3fs-virt/iovs");

  int deviceId = -1;
  ASSERT_EQ(cudaGetDevice(&deviceId), cudaSuccess);
  struct hf3fs_iov iov{};
  ASSERT_EQ(hf3fs_iovcreate_device(&iov, tmpDir.path(), 4096, 0, deviceId), 0);
  SCOPE_EXIT {
    if (iov.iovh) {
      hf3fs_iovdestroy(&iov);
    }
  };

  auto link = publicationPath(iov, deviceId);
  EXPECT_TRUE(isSymlink(link));
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_DEVICE);

  hf3fs_iovdestroy(&iov);
  EXPECT_FALSE(isSymlink(link));
}

TEST_F(TestUsrbIoGdrFixture, ExplicitUnlinkPreventsDestroyFromRemovingReplacementPublication) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No CUDA IPC-capable GPU available";
  }

  TmpDir tmpDir;
  ASSERT_TRUE(tmpDir.valid());
  std::filesystem::create_directories(std::string(tmpDir.path()) + "/3fs-virt/iovs");

  int deviceId = -1;
  ASSERT_EQ(cudaGetDevice(&deviceId), cudaSuccess);
  struct hf3fs_iov iov{};
  ASSERT_EQ(hf3fs_iovcreate_device(&iov, tmpDir.path(), 4096, 0, deviceId), 0);
  SCOPE_EXIT {
    if (iov.iovh) {
      hf3fs_iovdestroy(&iov);
    }
  };

  auto link = publicationPath(iov, deviceId);
  hf3fs_iovunlink(&iov);
  ASSERT_FALSE(isSymlink(link));
  ASSERT_EQ(symlink("gdr://replacement-publication", link.c_str()), 0);

  hf3fs_iovdestroy(&iov);
  EXPECT_EQ(iov.iovh, nullptr);
  EXPECT_TRUE(isSymlink(link));
  EXPECT_EQ(unlink(link.c_str()), 0);
}

TEST_F(TestUsrbIoGdrFixture, DestroyRetainsGpuHandleWhenPublicationUnlinkFails) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No CUDA IPC-capable GPU available";
  }

  TmpDir tmpDir;
  ASSERT_TRUE(tmpDir.valid());
  std::filesystem::create_directories(std::string(tmpDir.path()) + "/3fs-virt/iovs");

  int deviceId = -1;
  ASSERT_EQ(cudaGetDevice(&deviceId), cudaSuccess);
  struct hf3fs_iov iov{};
  ASSERT_EQ(hf3fs_iovcreate_device(&iov, tmpDir.path(), 4096, 0, deviceId), 0);
  SCOPE_EXIT {
    if (iov.iovh) {
      hf3fs_iovdestroy(&iov);
    }
  };

  auto link = publicationPath(iov, deviceId);
  ASSERT_EQ(unlink(link.c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(link));

  auto *handle = iov.iovh;
  hf3fs_iovdestroy(&iov);
  EXPECT_EQ(iov.iovh, handle);
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_DEVICE);

  ASSERT_TRUE(std::filesystem::remove(link));
  hf3fs_iovdestroy(&iov);
  EXPECT_EQ(iov.iovh, nullptr);
}

TEST_F(TestUsrbIoGdrFixture, ImporterProcessKeepsPublisherPublication) {
  const char *mount = std::getenv("HF3FS_GDR_IMPORT_MOUNT");
  if (!mount) {
    GTEST_SKIP() << "Subprocess-only importer check";
  }

  const char *idText = std::getenv("HF3FS_GDR_IMPORT_ID");
  const char *sizeText = std::getenv("HF3FS_GDR_IMPORT_SIZE");
  const char *deviceText = std::getenv("HF3FS_GDR_IMPORT_DEVICE");
  const char *expectedText = std::getenv("HF3FS_GDR_IMPORT_EXPECTED_BYTE");
  ASSERT_NE(idText, nullptr);
  ASSERT_NE(sizeText, nullptr);
  ASSERT_NE(deviceText, nullptr);
  ASSERT_NE(expectedText, nullptr);

  auto parsedId = hf3fs::Uuid::fromHexString(idText);
  ASSERT_OK(parsedId);
  std::array<uint8_t, 16> id{};
  std::memcpy(id.data(), parsedId->data, id.size());
  auto size = static_cast<size_t>(std::strtoull(sizeText, nullptr, 10));
  auto deviceId = std::atoi(deviceText);
  auto expectedByte = static_cast<uint8_t>(std::atoi(expectedText));

  struct hf3fs_iov importer{};
  ASSERT_EQ(hf3fs_iovopen_device(&importer, id.data(), mount, size, 0, deviceId), 0);
  SCOPE_EXIT {
    if (importer.iovh) {
      hf3fs_iovdestroy(&importer);
    }
  };

  auto link = publicationPath(importer, deviceId);
  ASSERT_TRUE(isSymlink(link));
  EXPECT_EQ(hf3fs_iovsync(&importer, 0), 0);
  uint8_t actual = 0;
  ASSERT_EQ(cudaMemcpy(&actual, importer.base, 1, cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_EQ(actual, expectedByte);

  hf3fs_iovdestroy(&importer);
  EXPECT_TRUE(isSymlink(link));
}

TEST_F(TestUsrbIoGdrFixture, WrapPublishesBaseOffsetAndSupportsMultipleNonOwningImporters) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No CUDA IPC-capable GPU available";
  }
  if (currentExecutable().empty()) {
    GTEST_SKIP() << "Subprocess execution through /proc/self/exe is unavailable";
  }

  TmpDir tmpDir;
  ASSERT_TRUE(tmpDir.valid());
  std::filesystem::create_directories(std::string(tmpDir.path()) + "/3fs-virt/iovs");

  int deviceId = -1;
  ASSERT_EQ(cudaGetDevice(&deviceId), cudaSuccess);
  void *allocation = nullptr;
  ASSERT_EQ(cudaMalloc(&allocation, 4096), cudaSuccess);
  SCOPE_EXIT {
    if (allocation) {
      EXPECT_EQ(cudaFree(allocation), cudaSuccess);
    }
  };
  ASSERT_EQ(cudaMemset(allocation, 0x11, 4096), cudaSuccess);
  auto *view = static_cast<uint8_t *>(allocation) + 512;
  ASSERT_EQ(cudaMemset(view, 0x7A, 1024), cudaSuccess);
  ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);

  struct hf3fs_iov publisher{};
  auto id = hf3fs::Uuid::random();
  ASSERT_EQ(hf3fs_iovwrap_device(&publisher,
                                 view,
                                 reinterpret_cast<const uint8_t *>(id.data),
                                 tmpDir.path(),
                                 1024,
                                 0,
                                 deviceId),
            0);
  SCOPE_EXIT {
    if (publisher.iovh) {
      hf3fs_iovdestroy(&publisher);
    }
  };

  auto link = publicationPath(publisher, deviceId);
  ASSERT_TRUE(isSymlink(link));
  auto target = std::filesystem::read_symlink(link).string();
  auto parsed = hf3fs::lib::parseGdrUri(target);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->deviceId, deviceId);
  EXPECT_EQ(parsed->offset, 512u);
  EXPECT_EQ(parsed->size, 1024u);
  EXPECT_GE(parsed->allocationSize, parsed->offset + parsed->size);

  EXPECT_TRUE(runImporterProcess(publisher, deviceId, 0x7A));
  EXPECT_TRUE(isSymlink(link));
  EXPECT_TRUE(runImporterProcess(publisher, deviceId, 0x7A));
  EXPECT_TRUE(isSymlink(link));

  hf3fs_iovdestroy(&publisher);
  EXPECT_FALSE(isSymlink(link));

  uint8_t actual = 0;
  ASSERT_EQ(cudaMemcpy(&actual, view, 1, cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_EQ(actual, 0x7A);
  ASSERT_EQ(cudaFree(allocation), cudaSuccess);
  allocation = nullptr;
}

// ==========================================================================
// REQ-L4-002: iovopen/iovwrap host path; _device variants for GPU
// ==========================================================================

// @tests SCN-L4-002-00b
TEST_F(TestUsrbIoGdrFixture, SCN_L4_002_00b_IovWrapNegativeNumaHostPath) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  uint8_t id[16] = {};
  // Use a valid host pointer so iovwrap succeeds (it doesn't validate mount existence)
  uint8_t buf[64] = {};

  // WHEN: iovwrap with negative numa (= host path, no NUMA binding)
  int rc = hf3fs_iovwrap(&iov, buf, id, "/nonexistent", sizeof(buf), 0, -1);

  // THEN: Succeeds (iovwrap doesn't validate mount), host path, no GPU dispatch
  EXPECT_EQ(rc, 0);
  // Verify it's host memory, not GPU
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_HOST);
}

TEST_F(TestUsrbIoGdrFixture, PrepIoUsesOverflowSafeOpaquePointerRanges) {
  struct hf3fs_ior ior{};
  ior.iorh = reinterpret_cast<void *>(uintptr_t{1});
  ior.for_read = true;
  struct hf3fs_iov iov{};

  iov.base = reinterpret_cast<uint8_t *>(uintptr_t{0x1000});
  iov.size = 0x100;
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, nullptr, 0, 0, 1, nullptr), -EINVAL);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, iov.base, 0, 0, 0, nullptr), -EINVAL);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(uintptr_t{0x0fff}), 0, 0, 1, nullptr), -EINVAL);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(uintptr_t{0x1100}), 0, 0, 1, nullptr), -EINVAL);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(uintptr_t{0x10ff}), 0, 0, 2, nullptr), -EINVAL);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(uintptr_t{0x10ff}), 0, 0, 1, nullptr), -EBADF);

  auto maxAddress = std::numeric_limits<uintptr_t>::max();
  iov.base = reinterpret_cast<uint8_t *>(maxAddress - 0xff);
  iov.size = 0xff;
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(maxAddress - 8), 0, 0, 8, nullptr), -EBADF);
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, reinterpret_cast<void *>(maxAddress - 8), 0, 0, 9, nullptr), -EINVAL);

  iov.base = reinterpret_cast<uint8_t *>(maxAddress - 7);
  iov.size = 8;
  EXPECT_EQ(hf3fs_prep_io(&ior, &iov, true, iov.base, 0, 0, 1, nullptr), -EINVAL);
}

// @tests SCN-L4-002-01
TEST_F(TestUsrbIoGdrFixture, SCN_L4_002_01_IovOpenDeviceNoGdr) {
  if (hasGpu()) {
    GTEST_SKIP() << "Test for non-GPU environment";
  }

  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  uint8_t id[16] = {};

  // WHEN: iovopen_device, GDR unavailable
  int rc = hf3fs_iovopen_device(&iov, id, "/nonexistent", 4096, 0, 0);

  // THEN: Returns -ENOTSUP
  EXPECT_EQ(rc, -ENOTSUP);
}

// @tests SCN-L4-002-02
TEST_F(TestUsrbIoGdrFixture, SCN_L4_002_02_IovWrapDeviceNoGdr) {
  if (hasGpu()) {
    GTEST_SKIP() << "Test for non-GPU environment";
  }

  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  uint8_t id[16] = {};
  void *fakePtr = reinterpret_cast<void *>(0x1000);

  // WHEN: iovwrap_device, GDR unavailable
  int rc = hf3fs_iovwrap_device(&iov, fakePtr, id, "/nonexistent", 4096, 0, 0);

  // THEN: Returns -ENOTSUP
  EXPECT_EQ(rc, -ENOTSUP);
}

// ==========================================================================
// REQ-L4-003: Unified iovdestroy Dispatch
// ==========================================================================

// @tests SCN-L4-003-02
TEST_F(TestUsrbIoGdrFixture, SCN_L4_003_02_DestroyHostIov) {
  // GIVEN: A zeroed iov (host-like, numa >= 0)
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = 0;

  // WHEN: iovdestroy is called
  // THEN: No crash (best-effort cleanup on zeroed iov)
  hf3fs_iovdestroy(&iov);

  // Verify iov was zeroed/cleaned
  EXPECT_EQ(iov.base, nullptr);
}

// ==========================================================================
// REQ-L4-004: Query Functions
// ==========================================================================

// @tests SCN-L4-004-01
TEST_F(TestUsrbIoGdrFixture, SCN_L4_004_01_MemTypeHostIov) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = 0;

  // WHEN: mem_type query
  enum hf3fs_mem_type type = hf3fs_iov_mem_type(&iov);

  // THEN: HF3FS_MEM_HOST
  EXPECT_EQ(type, HF3FS_MEM_HOST);
}

// @tests SCN-L4-004-01
TEST_F(TestUsrbIoGdrFixture, SCN_L4_004_01_DeviceIdHostIov) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = 0;

  // WHEN: device_id query on host iov
  int devId = hf3fs_iov_device_id(&iov);

  // THEN: Returns -1
  EXPECT_EQ(devId, -1);
}

// @tests SCN-L4-004-02
TEST_F(TestUsrbIoGdrFixture, SCN_L4_004_02_GdrAvailableLazyInit) {
  // WHEN: hf3fs_gdr_available is called
  bool avail = hf3fs_gdr_available();

  // THEN: Returns a valid boolean, consistent across calls
  bool avail2 = hf3fs_gdr_available();
  EXPECT_EQ(avail, avail2);
}

// @tests REQ-L4-004
TEST_F(TestUsrbIoGdrFixture, GdrDeviceCount) {
  int count = hf3fs_gdr_device_count();
  if (hasGpu()) {
    EXPECT_GT(count, 0);
  } else {
    EXPECT_EQ(count, 0);
  }
}

// ==========================================================================
// REQ-L4-006: iovsync Dispatch
// ==========================================================================

// @tests SCN-L4-006-02
TEST_F(TestUsrbIoGdrFixture, SCN_L4_006_02_SyncHostIov) {
  // GIVEN: Host iov
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = 0;

  // WHEN: iovsync is called
  int rc = hf3fs_iovsync(&iov, 1);

  // THEN: Returns 0 (no-op for host)
  EXPECT_EQ(rc, 0);
}

// @tests SCN-L4-006-02
TEST_F(TestUsrbIoGdrFixture, SyncHostIovDirection0) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = 0;

  int rc = hf3fs_iovsync(&iov, 0);
  EXPECT_EQ(rc, 0);
}

// ==========================================================================
// REQ-L3-005: GDR URI Parsing (Pure Logic)
// ==========================================================================

// @tests SCN-L3-005-01
TEST_F(TestUsrbIoGdrFixture, SCN_L3_005_01_ValidUriFormatThroughIovopen) {
  // GIVEN: A valid URI in symlink form
  // Build a proper URI and verify format through the API path
  uint8_t ipcHandle[64];
  for (int i = 0; i < 64; i++) ipcHandle[i] = static_cast<uint8_t>(i * 3 + 7);

  std::string uri = buildGdrUri(0, 1073741824, 0, 1073741824, ipcHandle);

  // Verify URI has correct format
  EXPECT_EQ(uri.substr(0, 14), "gdr://v2/devic");
  EXPECT_NE(uri.find("/device/0/"), std::string::npos);
  EXPECT_NE(uri.find("/allocation/1073741824/"), std::string::npos);
  EXPECT_NE(uri.find("/offset/0/"), std::string::npos);
  EXPECT_NE(uri.find("/size/1073741824/"), std::string::npos);
  EXPECT_NE(uri.find("/ipc/"), std::string::npos);
  // IPC hex should be 128 chars
  size_t ipcPos = uri.find("/ipc/");
  ASSERT_NE(ipcPos, std::string::npos);
  std::string hexPart = uri.substr(ipcPos + 5);
  EXPECT_EQ(hexPart.size(), 128u);

  // On GPU machines, test through iovopen with a crafted symlink
  if (hasGpu()) {
    TmpDir tmpDir;
    if (tmpDir.valid()) {
      // Create the symlink structure: {mount}/3fs-virt/iovs/{uuid}.gdr.d0 -> uri
      std::string virtDir = std::string(tmpDir.path()) + "/3fs-virt/iovs";
      std::filesystem::create_directories(virtDir);

      // Create a symlink with the URI
      std::string symlinkPath = virtDir + "/deadbeef12345678deadbeef12345678.gdr.d0";
      symlink(uri.c_str(), symlinkPath.c_str());

      // Try to open — will fail because UUID doesn't match, but exercises parser
      struct hf3fs_iov iov;
      memset(&iov, 0, sizeof(iov));
      uint8_t id[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78, 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78};
      int rc = hf3fs_iovopen_device(&iov, id, tmpDir.path(), 1073741824, 0, 0);
      // May fail for various reasons, but should not crash
      (void)rc;
    }
  }
}

// ==========================================================================
// REQ-L3-003: GPU IOV Wrap (External Memory)
// ==========================================================================

// @tests SCN-L3-003-01
TEST_F(TestUsrbIoGdrFixture, SCN_L3_003_01_WrapExternalGpuPtr) {
  if (!hasGpu()) {
    // On CPU-only, iovwrap_device returns -ENOTSUP
    struct hf3fs_iov iov;
    memset(&iov, 0, sizeof(iov));
    uint8_t id[16] = {};
    void *fakePtr = reinterpret_cast<void *>(0x2000);
    int rc = hf3fs_iovwrap_device(&iov, fakePtr, id, "/nonexistent", 4096, 0, 0);
    EXPECT_EQ(rc, -ENOTSUP);
    return;
  }

  // On GPU: wrap would need real GPU pointer + mount
  GTEST_SKIP() << "Requires real GPU memory and mount — integration test only";
}

// ==========================================================================
// INV-GDR-001: internal handle registry is the polymorphism discriminant
// ==========================================================================

// @tests INV-GDR-001
TEST_F(TestUsrbIoGdrFixture, INV_GDR_001_PolymorphismSafety) {
  // GIVEN: An iov with the former magic NUMA value but no registered handle
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = -0x6472;
  iov.iovh = nullptr;

  // WHEN: Query operations are called
  // THEN: They must not crash and must recognize this as non-GPU
  enum hf3fs_mem_type type = hf3fs_iov_mem_type(&iov);
  EXPECT_EQ(type, HF3FS_MEM_HOST);

  int devId = hf3fs_iov_device_id(&iov);
  EXPECT_EQ(devId, -1);

  // Sync remains a host no-op because NUMA does not classify the iov.
  int rc = hf3fs_iovsync(&iov, 0);
  EXPECT_EQ(rc, 0);
}

// @tests INV-GDR-002
TEST_F(TestUsrbIoGdrFixture, INV_GDR_002_NumaDoesNotClassifyGpuIov) {
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));

  iov.numa = 0;
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_HOST);

  iov.numa = -0x6472;
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_HOST);
  EXPECT_EQ(hf3fs_iov_device_id(&iov), -1);
}
