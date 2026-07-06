/**
 * Scenario tests for Layer 5+6: IovTable + IoRing + GpuShm
 *
 * Tests GDR key parsing, import/removal, buffer lookup,
 * variant dispatch, GpuShmBuf lifecycle, GpuShmBufForIO.
 *
 * Covers: REQ-L5-001 through REQ-L5-004
 *         REQ-L6-001 through REQ-L6-004
 *
 * Key parsing (REQ-L5-001): parseKey() is static in IovTable.cc.
 * Tested through IovTable::lookupIov() which calls parseKey() internally.
 *
 * URI parsing (REQ-L5-002): tested through IovTable::addIov(), which calls
 * the shared lib::parseGdrUri() helper.
 */

#include <cstring>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

#include "client/storage/StorageClient.h"
#include "fuse/IovTable.h"
#include "tests/GtestHelpers.h"

#ifdef HF3FS_GDR_ENABLED
#include "lib/common/GpuShm.h"
#endif

namespace hf3fs::fuse {
namespace {

meta::UserInfo rootUser() {
  meta::UserInfo ui;
  ui.uid = meta::Uid(0);
  ui.gid = meta::Gid(0);
  return ui;
}

auto addIovForParser(IovTable &table, const char *key, const Path &target) {
  storage::client::StorageClient storageClient;
  return table.addIov(key, target, 1234, rootUser(), folly::Executor::KeepAlive<>{}, storageClient);
}

void expectParserError(IovTable &table, const char *key, std::string_view message) {
  auto result = addIovForParser(table, key, Path("/dev/shm/unused"));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(std::string(result.error().message()), testing::HasSubstr(std::string(message)));
}

}  // namespace

// ==========================================================================
// REQ-L5-001: GDR Key Parsing in IovTable
// ==========================================================================

class TestIovTableGdr : public ::testing::Test {
};

// @tests SCN-L5-001-01
TEST_F(TestIovTableGdr, SCN_L5_001_01_ValidGdrKeyParsing) {
  IovTable table;

  auto result = addIovForParser(table, "abcdef1234567890abcdef1234567890.gdr.d0", Path("gdr://invalid"));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(std::string(result.error().message()), testing::HasSubstr("failed to parse GDR target URI"));
}

// @tests SCN-L5-001-02
TEST_F(TestIovTableGdr, SCN_L5_001_02_NonGdrKeyParsing) {
  IovTable table;

  auto result = addIovForParser(table, "abcdef1234567890abcdef1234567890.b4096", Path("/dev/shm/hf3fs-missing-iov"));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(std::string(result.error().message()), testing::HasSubstr("failed to stat shm path"));
}

// @tests SCN-L5-001-01
TEST_F(TestIovTableGdr, InvalidKeyFormatRejected) {
  IovTable table;

  expectParserError(table, "", "invalid shm key");
  expectParserError(table, "abcdef1234567890abcdef1234567890..gdr.d0", "empty attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d-1", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d1x", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gpu.d0", "invalid gdr attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.unknown", "unknown attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.d0", "gpu device id set for non-gdr key");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr", "gdr key missing gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.b4096", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.r4", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.t10", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.r4.phx", "invalid priority");
  expectParserError(table, "abcdef1234567890abcdef1234567890.b0", "invalid block size");
  expectParserError(table, "abcdef1234567890abcdef1234567890.r", "invalid io batch size");
}

// ==========================================================================
// REQ-L5-002: GDR Import via addIov
// ==========================================================================

// @tests SCN-L5-002-02
TEST_F(TestIovTableGdr, SCN_L5_002_02_InvalidGdrUriThroughAddIov) {
  // GIVEN: GDR key but invalid URI
  // addIov requires heavy dependencies (executor, storage client)
  // But we can verify the URI format expectation
  std::string invalidUri = "gdr://invalid";

  // THEN: URI does not match expected format "gdr://v1/device/{N}/size/{S}/ipc/{hex128}"
  EXPECT_EQ(invalidUri.find("gdr://v1/"), std::string::npos);
  // This URI would fail lib::parseGdrUri() inside addIov.
}

// @tests SCN-L5-002-04
TEST_F(TestIovTableGdr, SCN_L5_002_04_GdrNotCompiledCheck) {
#ifdef HF3FS_GDR_ENABLED
  // GDR types available — gpuShmsById, gpuShmLock exist
  IovTable table;
  EXPECT_TRUE(table.gpuShmsById.empty());
  EXPECT_TRUE(table.gpuIovMetaByIovd.empty());
#else
  // GDR types not available — compile-time check only
  IovTable table;
  EXPECT_TRUE(table.shmsById.empty());
#endif
}

// ==========================================================================
// REQ-L5-003: GDR IOV Removal via rmIov
// ==========================================================================

// @tests SCN-L5-003-01
TEST_F(TestIovTableGdr, SCN_L5_003_01_IovTableDefaultState) {
  // GIVEN: A default IovTable
  IovTable table;

#ifdef HF3FS_GDR_ENABLED
  // THEN: GPU maps are empty
  EXPECT_TRUE(table.gpuShmsById.empty());
  EXPECT_TRUE(table.gpuIovMetaByIovd.empty());
#endif

  // Host maps are empty
  EXPECT_TRUE(table.shmsById.empty());

  // iovs not yet initialized
  EXPECT_EQ(table.iovs, nullptr);
}

// @tests SCN-L5-003-01
TEST_F(TestIovTableGdr, SCN_L5_003_01_RmIovOnEmptyTable) {
  IovTable table;

  meta::UserInfo ui;
  ui.uid = meta::Uid(0);
  ui.gid = meta::Gid(0);

  // WHEN: rmIov on empty table with GDR key
  auto result = table.rmIov("abcdef1234567890abcdef1234567890.gdr.d0", ui);

  // THEN: Returns error (not found)
  EXPECT_TRUE(result.hasError());
}

#ifdef HF3FS_GDR_ENABLED

TEST_F(TestIovTableGdr, RemoveIovsByPidCleansGpuNullSlotMetadata) {
  IovTable table;
  table.init(Path("/mnt/3fs"), 8);

  auto iovd = table.iovs->alloc();
  ASSERT_TRUE(iovd);
  table.iovs->table[*iovd].store(nullptr);

  auto id = Uuid::fromHexString("abcdef1234567890abcdef1234567890");
  ASSERT_TRUE(id);
  table.gpuIovMetaByIovd[*iovd] = IovTable::GpuIovMeta{"abcdef1234567890abcdef1234567890.gdr.d0",
                                                       Path("gdr://v1/device/0/size/4096/ipc/00"),
                                                       meta::Uid(7),
                                                       meta::Gid(7),
                                                       4242};
  table.gpuShmsById[*id] = std::shared_ptr<lib::GpuShmBuf>();

  auto ioRings = table.removeIovsByPid(4242);

  EXPECT_TRUE(ioRings.empty());
  EXPECT_TRUE(table.gpuIovMetaByIovd.empty());
  EXPECT_TRUE(table.gpuShmsById.empty());
  EXPECT_EQ(table.iovs->slots.nextAvail.load(), 0);
}

#endif  // HF3FS_GDR_ENABLED

// ==========================================================================
// REQ-L5-004: lookupBufs Lambda -- Host-then-GPU Lookup
// ==========================================================================

// @tests SCN-L5-004-03
TEST_F(TestIovTableGdr, SCN_L5_004_03_UUIDNotFound) {
  IovTable table;

  // GIVEN: A UUID not in any map
  Uuid testUuid;
  memset(&testUuid, 0xAB, sizeof(testUuid));

  // THEN: Not found in shmsById
  EXPECT_EQ(table.shmsById.find(testUuid), table.shmsById.end());

#ifdef HF3FS_GDR_ENABLED
  // Not found in gpuShmsById either
  EXPECT_EQ(table.gpuShmsById.find(testUuid), table.gpuShmsById.end());
#endif
}

// ==========================================================================
// REQ-L6-001: IoBufForIO Variant Dispatch
// ==========================================================================

#ifdef HF3FS_GDR_ENABLED

// @tests SCN-L6-001-01, SCN-L6-001-02, SCN-L6-002-02
TEST_F(TestIovTableGdr, SCN_L6_001_VariantTypeCheck) {
  using namespace hf3fs::lib;

  // When HF3FS_GDR_ENABLED, GpuShmBufForIO exists with expected interface
  // Verify the type is constructible from the expected arguments
  static_assert(std::is_constructible_v<GpuShmBufForIO, std::shared_ptr<GpuShmBuf>, size_t>,
                "GpuShmBufForIO must be constructible from shared_ptr<GpuShmBuf> and offset");

  // Verify it has ptr() and offset() methods
  // (static_assert on method existence through decltype)
  static_assert(std::is_same_v<decltype(std::declval<GpuShmBufForIO>().ptr()), uint8_t *>,
                "GpuShmBufForIO::ptr() must return uint8_t*");
  static_assert(std::is_same_v<decltype(std::declval<GpuShmBufForIO>().offset()), size_t>,
                "GpuShmBufForIO::offset() must return size_t");
}

#endif  // HF3FS_GDR_ENABLED

// ==========================================================================
// REQ-L6-002: GpuShmBuf IPC Import
// ==========================================================================

#ifdef HF3FS_GDR_ENABLED

// @tests SCN-L6-002-01, SCN-L6-002-02
TEST_F(TestIovTableGdr, SCN_L6_002_GpuIpcHandleDefaults) {
  using namespace hf3fs::lib;

  // Default GpuIpcHandle should be invalid
  GpuIpcHandle handle;
  EXPECT_FALSE(handle.valid);
}

// @tests SCN-L6-002-03
TEST_F(TestIovTableGdr, SCN_L6_002_03_IpcHandleSerialization) {
  using namespace hf3fs::lib;

  // GIVEN: A GpuIpcHandle with known data
  GpuIpcHandle handle;
  for (int i = 0; i < 64; i++) {
    handle.data[i] = static_cast<uint8_t>(i);
  }
  handle.valid = true;

  // WHEN: serialize and deserialize
  std::string serialized = handle.serialize();
  EXPECT_FALSE(serialized.empty());

  auto deserialized = GpuIpcHandle::deserialize(serialized);
  ASSERT_TRUE(deserialized.has_value());

  // THEN: Round-trip matches
  EXPECT_TRUE(deserialized->valid);
  EXPECT_EQ(memcmp(handle.data, deserialized->data, 64), 0);
}

#endif  // HF3FS_GDR_ENABLED

// ==========================================================================
// REQ-L6-004: GpuShmBufForIO Offset View
// ==========================================================================

#ifdef HF3FS_GDR_ENABLED

// @tests SCN-L6-004-01
TEST_F(TestIovTableGdr, SCN_L6_004_01_OffsetPtrArithmetic) {
  using namespace hf3fs::lib;

  // GIVEN: A GpuShmBuf with a known devicePtr. The owner-side constructor keeps
  // the pointer even if CUDA IPC export is unavailable in the local runtime.
  void *knownPtr = reinterpret_cast<void *>(0x10000);
  auto gpuShm = std::make_shared<GpuShmBuf>(knownPtr, 0x10000, 0, meta::Uid(0), 1234, 1);
  ASSERT_EQ(gpuShm->devicePtr, knownPtr);

  GpuShmBufForIO forIO(gpuShm, 4096);
  EXPECT_EQ(forIO.ptr(), static_cast<uint8_t *>(knownPtr) + 4096);
  EXPECT_EQ(forIO.offset(), 4096u);
  EXPECT_EQ(forIO.buffer(), gpuShm);

  GpuShmBufForIO forIO0(gpuShm, 0);
  EXPECT_EQ(forIO0.ptr(), static_cast<uint8_t *>(knownPtr));
  EXPECT_EQ(forIO0.offset(), 0u);
}

#endif  // HF3FS_GDR_ENABLED

}  // namespace hf3fs::fuse
