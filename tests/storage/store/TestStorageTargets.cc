#include <folly/experimental/TestUtil.h>

#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/SysResource.h"
#include "storage/store/StorageTargets.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage {
namespace {

TEST(TestStorageTargets, Normal) {
  folly::test::TemporaryDirectory tmpPath;

  StorageTargets::Config config;
  config.set_target_num_per_path(4);
  config.set_target_paths({tmpPath.path()});
  config.set_allow_disk_without_uuid(true);

  {
    AtomicallyTargetMap targetMap;
    StorageTargets targets(config, targetMap);
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{1}));
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{5}));

    StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list({1_MB});
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids({1, 2, 3, 4});
    ASSERT_OK(targets.create(createConfig));
    ASSERT_OK(targetMap.snapshot()->getTarget(TargetId{1}));
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{5}));
  }

  {
    AtomicallyTargetMap targetMap;
    StorageTargets targets(config, targetMap);
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{1}));
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{5}));

    CPUExecutorGroup executor(8, "");
    ASSERT_OK(targets.load(executor));
    ASSERT_OK(targetMap.snapshot()->getTarget(TargetId{1}));
    ASSERT_FALSE(targetMap.snapshot()->getTarget(TargetId{5}));

    ASSERT_OK(targetMap.offlineTargets(tmpPath.path()));
    auto target = targetMap.snapshot()->getTarget(TargetId{1});
    ASSERT_OK(target);
    ASSERT_TRUE((*target)->diskError);

    auto result = targetMap.offlineTarget(TargetId{1});
    ASSERT_TRUE(result.hasError());
    ASSERT_EQ(result.error().code(), StorageCode::kTargetOffline);
  }
}

}  // namespace
}  // namespace hf3fs::storage
