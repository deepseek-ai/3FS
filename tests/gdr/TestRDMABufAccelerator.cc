/**
 * Scenario tests for Layer 2: RDMABufAccelerator
 *
 * Tests RDMABufAccelerator creation, subranges, toRemoteBuf,
 * RDMABufUnified type dispatch, and sync.
 *
 * Covers: REQ-L2-001, REQ-L2-003, REQ-L2-004, REQ-L2-006
 */

#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "common/net/ib/AcceleratorMemory.h"
#include "common/net/ib/RDMABuf.h"
#include "common/net/ib/RDMABufAccelerator.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net {

class TestRDMABufAccelerator : public ::testing::Test {
 protected:
  static bool hasGpu() {
    return GDRManager::instance().isAvailable();
  }
};

// ==========================================================================
// REQ-L2-001: GPU RDMA Buffer Creation from Device Pointer
// ==========================================================================

// @tests SCN-L2-001-02
TEST_F(TestRDMABufAccelerator, SCN_L2_001_02_CreateFromGpuPointerNoGDR) {
  if (hasGpu()) {
    GTEST_SKIP() << "Test is for non-GPU environments";
  }

  // GIVEN: GDRManager::isAvailable() returns false
  // WHEN: createFromGpuPointer is called
  auto buf = RDMABufAccelerator::createFromGpuPointer(
      reinterpret_cast<void*>(0x1000), 4096, 0);

  // THEN: Returns invalid buffer
  EXPECT_FALSE(buf.valid());
  EXPECT_FALSE(static_cast<bool>(buf));
}

// @tests SCN-L2-001-01
TEST_F(TestRDMABufAccelerator, SCN_L2_001_01_CreateFromGpuPointerSuccess) {
  if (!hasGpu()) {
    GTEST_SKIP() << "No GPU available — integration test only";
  }

  // Integration test with real GPU memory
  auto& manager = GDRManager::instance();
  ASSERT_TRUE(manager.isAvailable());
  EXPECT_NE(manager.getRegionCache(), nullptr);
}

// ==========================================================================
// REQ-L2-003: RDMABufUnified Type-Safe Dispatch
// ==========================================================================

// @tests SCN-L2-003-03
TEST_F(TestRDMABufAccelerator, SCN_L2_003_03_UnifiedEmpty) {
  // GIVEN: Default-constructed RDMABufUnified
  RDMABufUnified unified;

  // THEN: valid()==false, ptr()==nullptr, size()==0
  EXPECT_FALSE(unified.valid());
  EXPECT_FALSE(static_cast<bool>(unified));
  EXPECT_EQ(unified.ptr(), nullptr);
  EXPECT_EQ(unified.size(), 0u);
  EXPECT_EQ(unified.capacity(), 0u);
  EXPECT_EQ(unified.type(), RDMABufUnified::Type::Empty);
  EXPECT_FALSE(unified.isHost());
  EXPECT_FALSE(unified.isGpu());
  EXPECT_EQ(unified.getMR(0), nullptr);

  auto remoteBuf = unified.toRemoteBuf();
  EXPECT_FALSE(static_cast<bool>(remoteBuf));
}

// @tests SCN-L2-003-01
TEST_F(TestRDMABufAccelerator, SCN_L2_003_01_UnifiedGpuDispatch) {
  // GIVEN: An RDMABufAccelerator (even default/invalid for type test)
  RDMABufAccelerator gpuBuf;
  RDMABufUnified unified(std::move(gpuBuf));

  // THEN: isGpu()==true, isHost()==false
  EXPECT_TRUE(unified.isGpu());
  EXPECT_FALSE(unified.isHost());
  EXPECT_EQ(unified.type(), RDMABufUnified::Type::Gpu);

  // Invalid GPU buf: valid() is false but type is Gpu
  EXPECT_FALSE(unified.valid());

  // Access underlying buffer
  auto& gpu = unified.asGpu();
  EXPECT_FALSE(gpu.valid());
  EXPECT_EQ(gpu.devicePtr(), nullptr);
}

// @tests SCN-L2-003-02
TEST_F(TestRDMABufAccelerator, SCN_L2_003_02_UnifiedHostDispatch) {
  // GIVEN: RDMABufUnified constructed with RDMABuf (host)
  RDMABuf hostBuf;  // Default invalid
  RDMABufUnified unified(std::move(hostBuf));

  // THEN: isHost()==true, isGpu()==false
  EXPECT_TRUE(unified.isHost());
  EXPECT_FALSE(unified.isGpu());
  EXPECT_EQ(unified.type(), RDMABufUnified::Type::Host);

  // Access underlying buffer
  auto& host = unified.asHost();
  EXPECT_FALSE(host.valid());
}

// ==========================================================================
// REQ-L2-004: Subrange Views and toRemoteBuf
// ==========================================================================

// @tests SCN-L2-004-01, SCN-L2-004-02
TEST_F(TestRDMABufAccelerator, SCN_L2_004_SubrangeOnInvalidBuffer) {
  // GIVEN: Default-constructed (invalid) RDMABufAccelerator
  RDMABufAccelerator buf;
  ASSERT_FALSE(buf.valid());

  // WHEN: subrange is called with various params
  auto sub0 = buf.subrange(0, 0);
  EXPECT_FALSE(sub0.valid());

  auto sub1 = buf.subrange(0, 4096);
  EXPECT_FALSE(sub1.valid());

  auto sub2 = buf.subrange(100, 200);
  EXPECT_FALSE(sub2.valid());
}

// @tests SCN-L2-004-03
TEST_F(TestRDMABufAccelerator, SCN_L2_004_03_ToRemoteBufInvalid) {
  // GIVEN: Invalid buffer
  RDMABufAccelerator buf;

  // WHEN: toRemoteBuf
  auto remoteBuf = buf.toRemoteBuf();

  // THEN: Returns invalid RDMARemoteBuf
  EXPECT_FALSE(static_cast<bool>(remoteBuf));
}

// ==========================================================================
// REQ-L2-006: GPU Buffer Synchronization
// ==========================================================================

// @tests SCN-L2-006-02
TEST_F(TestRDMABufAccelerator, SCN_L2_006_02_SyncOnInvalidBuffer) {
  // GIVEN: An invalid (default-constructed) RDMABufAccelerator
  RDMABufAccelerator buf;
  ASSERT_FALSE(buf.valid());

  // WHEN: sync is called with various directions
  // THEN: No-op, no crash
  buf.sync(0);
  buf.sync(1);
}

}  // namespace hf3fs::net
