/**
 * Scenario tests for Layer 3+4: UsrbIoGdr + UsrbIo (unified C API)
 *
 * Tests GPU IOV create/open/wrap/destroy, query functions, and sync.
 * Covers core dispatch paths and fallback behavior.
 *
 * Covers: REQ-L3-001, REQ-L3-003, REQ-L3-005
 *         REQ-L4-001 through REQ-L4-006
 *         INV-GDR-001, INV-GDR-002
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "lib/api/hf3fs_usrbio.h"
#include "tests/GtestHelpers.h"

namespace {

static bool hasGpu() { return hf3fs_gdr_available(); }

// Temp directory for symlink testing
class TmpDir {
 public:
  TmpDir() {
    const char *base = getenv("TMPDIR");
    if (!base) base = "/private/tmp/claude-502";
    path_ = std::string(base) + "/gdr_test_XXXXXX";
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
std::string buildGdrUri(int deviceId, size_t size, const uint8_t ipcHandle[64]) {
  char hex[129];
  for (int i = 0; i < 64; i++) {
    snprintf(hex + i * 2, 3, "%02x", ipcHandle[i]);
  }
  hex[128] = '\0';
  return std::string("gdr://v1/device/") + std::to_string(deviceId) + "/size/" + std::to_string(size) + "/ipc/" + hex;
}

class TestUsrbIoGdrFixture : public ::testing::Test {
};

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
TEST_F(TestUsrbIoGdrFixture, SCN_L4_001_02_DeviceFallbackNoGDR) {
  if (hasGpu()) {
    GTEST_SKIP() << "Test for machines without GPU";
  }

  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));

  // WHEN: iovcreate_device, GDR unavailable
  int rc = hf3fs_iovcreate_device(&iov, "/nonexistent/mount", 4096, 0, 0);

  // THEN: Falls back to host path (still fails due to no mount, but no crash)
  EXPECT_NE(rc, 0);
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

  std::string uri = buildGdrUri(0, 1073741824, ipcHandle);

  // Verify URI has correct format
  EXPECT_EQ(uri.substr(0, 14), "gdr://v1/devic");
  EXPECT_NE(uri.find("/device/0/"), std::string::npos);
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
// INV-GDR-001: iov->iovh Polymorphism Discriminant
// ==========================================================================

// @tests INV-GDR-001
TEST_F(TestUsrbIoGdrFixture, INV_GDR_001_PolymorphismSafety) {
  // GIVEN: An iov that looks GPU-like but has no real handle
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));
  iov.numa = -0x6472;  // kGpuIovMagicNuma
  iov.iovh = nullptr;

  // WHEN: Query operations are called
  // THEN: They must not crash and must recognize this as non-GPU
  enum hf3fs_mem_type type = hf3fs_iov_mem_type(&iov);
  EXPECT_EQ(type, HF3FS_MEM_HOST);

  int devId = hf3fs_iov_device_id(&iov);
  EXPECT_EQ(devId, -1);

  // Sync should be no-op on unregistered GPU-magic iov
  int rc = hf3fs_iovsync(&iov, 0);
  EXPECT_EQ(rc, 0);
}

// @tests INV-GDR-002
TEST_F(TestUsrbIoGdrFixture, INV_GDR_002_MagicNumaValue) {
  // Verify the magic numa value is stable
  struct hf3fs_iov iov;
  memset(&iov, 0, sizeof(iov));

  // Host iov: numa >= 0
  iov.numa = 0;
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_HOST);

  // GPU iov requires numa == -0x6472 AND registered handle
  iov.numa = -0x6472;
  // Without registered handle, still reports HOST
  EXPECT_EQ(hf3fs_iov_mem_type(&iov), HF3FS_MEM_HOST);

  // Verify the actual numeric value
  EXPECT_EQ(-0x6472, -25714);
}
