#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>

#include "chunk_engine/src/cxx.rs.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/CoLockManager.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/RobinHood.h"
#include "fbs/mgmtd/HeartbeatInfo.h"
#include "fbs/storage/Common.h"
#include "storage/service/TargetMap.h"
#include "storage/store/StorageTarget.h"

namespace hf3fs::test {
struct StorageTargetsHelper;
}

namespace hf3fs::storage {

class StorageTargets {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(target_paths, std::vector<Path>{}, [](auto &vec) { return !vec.empty(); });
    CONFIG_ITEM(target_num_per_path, 0u);
    CONFIG_HOT_UPDATED_ITEM(collect_all_fds, true);
    CONFIG_HOT_UPDATED_ITEM(space_info_cache_timeout, 5_s);
    CONFIG_HOT_UPDATED_ITEM(allow_disk_without_uuid, false);
    CONFIG_HOT_UPDATED_ITEM(create_engine_path, true);
    CONFIG_OBJ(storage_target, StorageTarget::Config);
  };

  class CreateConfig : public ConfigBase<CreateConfig> {
    CONFIG_ITEM(target_ids, std::vector<flat::TargetId::UnderlyingType>{});
    CONFIG_ITEM(physical_file_count, 256u);
    CONFIG_ITEM(allow_disk_without_uuid, false);
    CONFIG_ITEM(allow_existing_targets, false);
    CONFIG_ITEM(chunk_size_list, (std::vector<Size>{512_KB, 1_MB, 2_MB, 4_MB, 16_MB, 64_MB}));
    CONFIG_ITEM(only_chunk_engine, false);
  };

  StorageTargets(const Config &config, AtomicallyTargetMap &targetMap)
      : config_(config),
        targetMap_(targetMap) {}
  ~StorageTargets();

  Result<Void> init(CPUExecutorGroup &executor);

  // create a batch of storage targets.
  Result<Void> create(const CreateConfig &createConfig);

  // create new storage target.
  Result<Void> create(const CreateTargetReq &req);

  // open a batch of storage targets.
  Result<Void> load(CPUExecutorGroup &executor);

  // load a target.
  Result<Void> loadTarget(const Path &targetPath);

  // get fd list.
  auto &fds() const { return fds_; }

  // get space info.
  Result<std::vector<SpaceInfo>> spaceInfos(bool force);

  // get target paths.
  auto &targetPaths() const { return targetPaths_; }

  // get manufacturers.
  auto &manufacturers() const { return manufacturers_; }

  // global file store.
  auto &globalFileStore() { return globalFileStore_; }

  // chunk engines.
  auto &engines() const { return engines_; }

  // remove target.
  Result<Void> removeChunkEngineTarget(ChainId chainId, uint32_t diskIndex) {
    auto &engine = *engines_[diskIndex];
    return ChunkEngine::removeAllChunks(engine, chainId);
  }

 private:
  friend struct test::StorageTargetsHelper;
  ConstructLog<"storage::StorageTargets"> constructLog_;
  const Config &config_;
  AtomicallyTargetMap &targetMap_;
  GlobalFileStore globalFileStore_;

  std::vector<Path> targetPaths_;
  std::vector<std::string> manufacturers_;
  std::map<Path, uint32_t> pathToDiskIndex_;
  std::vector<rust::Box<chunk_engine::Engine>> engines_;

  CoLockManager<> targetLocks_;
  RelativeTime spaceInfoUpdatedTime_;
  std::vector<SpaceInfo> cachedSpaceInfos_;

  std::vector<int> fds_;
};

}  // namespace hf3fs::storage
