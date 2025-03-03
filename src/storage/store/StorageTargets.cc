#include "storage/store/StorageTargets.h"

#include <boost/filesystem/operations.hpp>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <memory>
#include <sys/statvfs.h>
#include <unordered_map>

#include "chunk_engine/src/cxx.rs.h"
#include "common/monitor/Sample.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Duration.h"
#include "common/utils/LogCommands.h"
#include "common/utils/Result.h"
#include "common/utils/SysResource.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {

using namespace std::chrono_literals;

StorageTargets::~StorageTargets() { void(); }

Result<Void> StorageTargets::init(CPUExecutorGroup &executor) {
  auto diskInfoResult = SysResource::scanDiskInfo();
  RETURN_AND_LOG_ON_ERROR(diskInfoResult);
  std::unordered_map<uint32_t, std::string> deviceIdToManufacturer;
  for (auto &info : *diskInfoResult) {
    deviceIdToManufacturer[info.deviceId] = info.manufacturer;
  }

  targetPaths_ = config_.target_paths();
  for (auto &path : targetPaths_) {
    struct stat st;
    int succ = ::stat(path.c_str(), &st);
    if (succ != 0) {
      auto msg = fmt::format("stat {} failed: {}", path, errno);
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageStatFailed, std::move(msg));
    }
    manufacturers_.push_back(deviceIdToManufacturer[st.st_dev]);
  }

  uint32_t i = 0;
  for (auto &path : targetPaths_) {
    pathToDiskIndex_[path] = i++;
  }

  std::vector<folly::coro::TaskWithExecutor<Result<rust::Box<chunk_engine::Engine>>>> tasks;
  for (auto &path : targetPaths_) {
    auto engine_path = path / "engine";
    bool create = !boost::filesystem::exists(engine_path);
    create |= config_.create_engine_path();
    tasks.push_back(folly::coro::co_invoke([engine_path, create]() -> CoTryTask<rust::Box<chunk_engine::Engine>> {
                      std::string error;
                      auto engine = chunk_engine::create(engine_path.c_str(), create, sizeof(ChainId), error);
                      if (!error.empty()) {
                        co_return makeError(StorageCode::kStorageStatFailed, std::move(error));
                      }
                      co_return rust::Box<chunk_engine::Engine>::from_raw(engine);
                    }).scheduleOn(&executor.pickNext()));
  }

  auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));
  for (auto &result : results) {
    RETURN_AND_LOG_ON_ERROR(result);
    engines_.push_back(std::move(result.value()));
  }

  return Void{};
}

Result<Void> StorageTargets::create(const CreateConfig &createConfig) {
  CPUExecutorGroup executor(1, "Creator");
  RETURN_AND_LOG_ON_ERROR(init(executor));
  auto targetPaths = config_.target_paths();
  auto targetNumPerPath = config_.target_num_per_path();
  auto targetIdSize = createConfig.target_ids().size();
  if (targetPaths.empty()) {
    auto msg = fmt::format("List of target path is empty");
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  if (targetNumPerPath == 0) {
    auto msg = fmt::format("Target num per path is 0!");
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  if (targetPaths.size() * targetNumPerPath != targetIdSize) {
    auto msg = fmt::format("Unable to arrange target. path size {}, target num per path {}, target id size {}",
                           targetPaths.size(),
                           targetNumPerPath,
                           targetIdSize);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, msg);
  }

  size_t idx = 0;
  for (auto &targetId : createConfig.target_ids()) {
    auto diskIndex = idx / targetNumPerPath;
    auto storageTarget = StorageTarget::enable_shared_from_this::create(config_.storage_target(),
                                                                        globalFileStore_,
                                                                        diskIndex,
                                                                        &*engines_[diskIndex]);
    PhysicalConfig targetConfig;
    targetConfig.path = targetPaths[diskIndex] / std::to_string(targetId);
    targetConfig.target_id = targetId;
    targetConfig.allow_disk_without_uuid = createConfig.allow_disk_without_uuid();
    targetConfig.allow_existing_targets = createConfig.allow_existing_targets();
    targetConfig.physical_file_count = createConfig.physical_file_count();
    targetConfig.chunk_size_list = createConfig.chunk_size_list();
    targetConfig.only_chunk_engine = createConfig.only_chunk_engine();
    RETURN_AND_LOG_ON_ERROR(storageTarget->create(targetConfig));
    ++idx;
    RETURN_AND_LOG_ON_ERROR(targetMap_.addStorageTarget(std::move(storageTarget)));
  }
  return Void{};
}

Result<Void> StorageTargets::create(const CreateTargetReq &req) {
  if (req.diskIndex >= config_.target_paths().size()) {
    auto msg = fmt::format("disk index exceed {} >= {}", req.diskIndex, config_.target_paths().size());
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  if (req.chainId == ChainId{}) {
    auto msg = fmt::format("target {} without chain id", req.targetId);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }

  folly::coro::Baton baton;
  auto lock = targetLocks_.lock(baton, fmt::to_string(req.chainId));
  if (!lock.locked()) {
    folly::coro::blockingWait(lock.lock());
  }
  if (auto existingTarget = targetMap_.snapshot()->getByChainId(VersionedChainId{req.chainId, {}}, true)) {
    auto existingTargetId = (*existingTarget)->targetId;
    if (existingTargetId != req.targetId) {
      auto msg = fmt::format("target {} is existing with same chain id {}, req target {}",
                             existingTargetId,
                             req.chainId,
                             req.targetId);
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageInitFailed, std::move(msg));
    }
    if (req.addChunkSize) {
      RETURN_AND_LOG_ON_ERROR((*existingTarget)->storageTarget->addChunkSize(req.chunkSizeList));
    }
  } else if (req.addChunkSize) {
    auto msg = fmt::format("target {} {} is not existing", req.chainId, req.targetId);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  if (targetMap_.snapshot()->getTarget(req.targetId)) {
    if (req.allowExistingTarget) {
      auto targetPath = config_.target_paths()[req.diskIndex] / std::to_string(req.targetId);
      if (!boost::filesystem::exists(targetPath)) {
        auto msg = fmt::format("target {} is existing in memory, but not found in disk", req.targetId);
        XLOG(ERR, msg);
        return makeError(StorageCode::kStorageInitFailed, std::move(msg));
      }
      XLOGF(INFO, "target {} is already existing, return succ", req.targetId);
      return Void{};
    } else {
      auto msg = fmt::format("target {} is already existing", req.targetId);
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageInitFailed, std::move(msg));
    }
  }

  auto storageTarget = StorageTarget::enable_shared_from_this::create(config_.storage_target(),
                                                                      globalFileStore_,
                                                                      req.diskIndex,
                                                                      &*engines_[req.diskIndex]);
  PhysicalConfig targetConfig;
  auto targetPath = config_.target_paths()[req.diskIndex] / std::to_string(req.targetId);
  targetConfig.path = targetPath;
  targetConfig.target_id = req.targetId;
  targetConfig.chain_id = req.chainId;
  targetConfig.allow_disk_without_uuid = config_.allow_disk_without_uuid();
  targetConfig.allow_existing_targets = req.allowExistingTarget;
  targetConfig.physical_file_count = req.physicalFileCount;
  targetConfig.chunk_size_list = req.chunkSizeList;
  targetConfig.kv_store_type = config_.storage_target().kv_store().type();
  targetConfig.only_chunk_engine = req.onlyChunkEngine;
  RETURN_AND_LOG_ON_ERROR(storageTarget->create(targetConfig));
  XLOGF(INFO, "Create storage target {} at {}", storageTarget->targetId(), targetPath.string());
  RETURN_AND_LOG_ON_ERROR(targetMap_.addStorageTarget(std::move(storageTarget)));
  return Void{};
}

Result<Void> StorageTargets::load(CPUExecutorGroup &executor) {
  RETURN_AND_LOG_ON_ERROR(init(executor));
  std::vector<folly::coro::TaskWithExecutor<Result<Void>>> tasks;
  for (auto &parentPath : config_.target_paths()) {
    auto writable = CheckWorker::checkWritable(parentPath);
    if (!writable) {
      XLOGF(DFATAL, "path {} isn't writable, skip it", parentPath);
    }
    for (auto &targetPath : boost::filesystem::directory_iterator(parentPath)) {
      auto targetConfigPath = targetPath / kPhysicalConfigFileName;
      if (boost::filesystem::is_directory(targetPath) && boost::filesystem::is_regular_file(targetConfigPath)) {
        tasks.push_back(folly::coro::co_invoke([this, targetPath]() -> CoTryTask<Void> {
                          co_return loadTarget(targetPath);
                        }).scheduleOn(&executor.pickNext()));
      }
    }
  }
  auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));
  for (auto &result : results) {
    RETURN_AND_LOG_ON_ERROR(result);
  }
  if (config_.collect_all_fds()) {
    globalFileStore_.collect(fds_);
  }
  return Void{};
}

// load a target.
Result<Void> StorageTargets::loadTarget(const Path &targetPath) {
  auto diskPath = targetPath.parent_path();
  if (UNLIKELY(!pathToDiskIndex_.contains(diskPath))) {
    auto msg = fmt::format("Target path ({}) not belongs to any of disk paths", targetPath);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }

  auto diskIndex = pathToDiskIndex_[diskPath];
  auto storageTarget = StorageTarget::enable_shared_from_this::create(config_.storage_target(),
                                                                      globalFileStore_,
                                                                      diskIndex,
                                                                      &*engines_[diskIndex]);
  RETURN_AND_LOG_ON_ERROR(storageTarget->load(targetPath));
  XLOGF(INFO, "Load storage target {} at {}", storageTarget->targetId(), targetPath.string());
  auto targetId = storageTarget->targetId();
  if (UNLIKELY(targetPath.filename().string() != fmt::format("{}", targetId.toUnderType()))) {
    auto msg = fmt::format("Target id {} and path {} mismatch!", targetId, targetPath);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  RETURN_AND_LOG_ON_ERROR(targetMap_.addStorageTarget(std::move(storageTarget)));
  return Void{};
}

Result<std::vector<SpaceInfo>> StorageTargets::spaceInfos(bool force) {
  folly::coro::Baton baton;
  auto lock = targetLocks_.lock(baton, "spaceInfos");
  if (!lock.locked()) {
    folly::coro::blockingWait(lock.lock());
  }

  auto now = RelativeTime::now();
  auto elapsedTime = now - spaceInfoUpdatedTime_;
  if (elapsedTime < config_.space_info_cache_timeout() && !force) {
    return cachedSpaceInfos_;
  }

  std::unordered_map<std::string, uint64_t> diskUnusedSize;
  std::unordered_map<std::string, std::vector<hf3fs::flat::TargetId>> pathToTargetIds;
  auto snapshot = targetMap_.snapshot();
  for (auto &[targetId, target] : snapshot->getTargets()) {
    pathToTargetIds[target.path.parent_path().string()].emplace_back(targetId);
    if (target.storageTarget != nullptr) {
      diskUnusedSize[target.path.parent_path().string()] += target.storageTarget->unusedSize();
    }
  }

  std::vector<SpaceInfo> ret;
  for (auto &[path, index] : pathToDiskIndex_) {
    SpaceInfo info;
    info.path = targetPaths_[index].string();
    info.targetIds = pathToTargetIds[info.path];

    boost::system::error_code ec{};
    auto spaceInfo = boost::filesystem::space(path, ec);
    if (UNLIKELY(ec.failed())) {
      auto msg = fmt::format("get space info of directory {} failed: {}", path, ec.message());
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkOpenFailed, std::move(msg));
    }
    auto usedSize = engines_[index]->raw_used_size();
    info.capacity = spaceInfo.capacity;
    info.free = spaceInfo.free + diskUnusedSize[info.path] + usedSize.reserved_size;
    info.available = spaceInfo.available;
    info.manufacturer = manufacturers_[index];
    ret.push_back(std::move(info));
  }
  cachedSpaceInfos_ = ret;
  spaceInfoUpdatedTime_ = RelativeTime::now();
  return ret;
}

}  // namespace hf3fs::storage
