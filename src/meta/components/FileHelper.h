#pragma once

#include <chrono>
#include <fmt/core.h>
#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/experimental/coro/SharedMutex.h>
#include <memory>
#include <optional>
#include <utility>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "meta/base/Config.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

using FsStatus = StatFsRsp;

class FileHelper {
 public:
  using RetryOptions = storage::client::RetryOptions;

  FileHelper(const Config &config,
             std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient,
             std::shared_ptr<storage::client::StorageClient> storageClient)
      : config_(config),
        mgmtdClient_(std::move(mgmtdClient)),
        storageClient_(std::move(storageClient)) {}

  ~FileHelper() { stopAndJoin(); }

  void start(CPUExecutorGroup &exec);
  void stopAndJoin();

  CoTryTask<uint64_t> queryLength(const UserInfo &userInfo, const Inode &inode, bool *hasHole = nullptr);

  CoTryTask<size_t> remove(const UserInfo &userInfo,
                           const Inode &inode,
                           RetryOptions retry,
                           uint32_t removeChunksBatchSize);

  CoTryTask<FsStatus> statFs(const UserInfo &userInfo, std::chrono::milliseconds cacheDuration);

  std::optional<FsStatus> cachedFsStatus() const { return cachedFsStatus_.rlock()->status_; }

 private:
  CoTryTask<void> updateStatFs();

  const Config &config_;
  std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient_;
  std::shared_ptr<storage::client::StorageClient> storageClient_;

  struct CachedFsStatus {
    RelativeTime update_;
    std::optional<FsStatus> status_;
  };

  std::unique_ptr<BackgroundRunner> bgRunner_;
  folly::Synchronized<CachedFsStatus> cachedFsStatus_;
};

}  // namespace hf3fs::meta::server
