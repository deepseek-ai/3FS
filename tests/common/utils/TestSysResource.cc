#include <cstdlib>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

#include "common/serde/Serde.h"
#include "common/utils/SysResource.h"

namespace hf3fs {

TEST(TestSysResource, GetHostname) {
  auto result = SysResource::hostname();
  ASSERT_TRUE(result);
  XLOGF(INFO, "hostname is {}", result.value());
}

TEST(TestSysResource, FDLimit) {
  auto result = SysResource::increaseProcessFDLimit(8192);
  ASSERT_TRUE(result);
}

// todo: fix fs UUID in docker
TEST(TestSysResource, DISABLED_FileSystemUUID) {
  auto result = SysResource::fileSystemUUID();
  ASSERT_TRUE(result);
  auto &map = *result;
  ASSERT_TRUE(!map.empty());

  for (auto [deviceId, uuid] : map) {
    XLOGF(INFO, "device: {}, uuid: {}", deviceId, uuid);
  }
}

TEST(TestSysResource, ScanDiskInfo) {
  auto result = SysResource::scanDiskInfo();
  ASSERT_TRUE(result);
  XLOGF(INFO, "info is {}", serde::toJsonString(*result));
}

TEST(TestSysResource, getAllEnvs) {
  ::setenv("HF3FS_TEST_ONE", "ONE", 1);
  ::setenv("HF3FS_TEST_TWO", "TWO", 1);
  ::setenv("TEST_THREE", "THREE", 1);
  auto m = SysResource::getAllEnvs("HF3FS_");
  ASSERT_TRUE(!m.empty());

  for (const auto &[k, v] : m) XLOGF(INFO, "env {} = {}", k, v);

  ASSERT_TRUE(m.contains("HF3FS_TEST_ONE"));
  ASSERT_EQ(m["HF3FS_TEST_ONE"], "ONE");

  ASSERT_TRUE(m.contains("HF3FS_TEST_TWO"));
  ASSERT_EQ(m["HF3FS_TEST_TWO"], "TWO");

  ASSERT_TRUE(!m.contains("TEST_THREE"));
}

}  // namespace hf3fs
