#pragma once

#include <folly/experimental/coro/Mutex.h>

#include "MgmtdData.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CoroSynchronized.h"
#include "common/utils/DefaultRetryStrategy.h"
#include "core/app/ServerEnv.h"
#include "core/user/UserStoreEx.h"
#include "fbs/mgmtd/NodeInfo.h"
#include "fbs/mgmtd/PersistentNodeInfo.h"
#include "fdb/FDBRetryStrategy.h"
#include "mgmtd/store/MgmtdStore.h"

namespace hf3fs::mgmtd {
struct MgmtdConfig;

using ClientSessionMap = RHStringHashMap<ClientSession>;

struct MgmtdState {
  MgmtdState(std::shared_ptr<core::ServerEnv> env, const MgmtdConfig &config);
  ~MgmtdState();

  UtcTime utcNow();
  Result<Void> validateClusterId(const core::ServiceOperation &ctx, std::string_view clusterId);
  CoTryTask<void> validateAdmin(const core::ServiceOperation &ctx, const flat::UserInfo &userInfo);
  CoTask<std::optional<flat::MgmtdLeaseInfo>> currentLease(UtcTime now);
  flat::NodeId selfId() const;
  kv::FDBRetryStrategy createRetryStrategy();

  const std::shared_ptr<core::ServerEnv> env_;
  flat::NodeInfo selfNodeInfo_;
  flat::PersistentNodeInfo selfPersistentNodeInfo_;
  const MgmtdConfig &config_;

  MgmtdStore store_;
  core::UserStoreEx userStore_;

  class WriterMutexGuard {
   public:
    WriterMutexGuard(std::unique_lock<folly::coro::Mutex> mu, std::string_view m)
        : mu_(std::move(mu)),
          method_(m) {}
    WriterMutexGuard(WriterMutexGuard &&other) = default;
    ~WriterMutexGuard();

   private:
    std::unique_lock<folly::coro::Mutex> mu_;
    std::string_view method_;
    SteadyTime start_ = SteadyClock::now();
  };

  template <NameWrapper method>
  CoTask<WriterMutexGuard> coScopedLock() {
    auto mu = co_await writerMu_.co_scoped_lock();
    co_return WriterMutexGuard(std::move(mu), method);
  }

  CoroSynchronized<MgmtdData> data_;
  CoroSynchronized<ClientSessionMap> clientSessionMap_;

 private:
  // logical lock for protecting the whole processing of a writer operation
  // during which read-modify-write will be performed on `data_`
  folly::coro::Mutex writerMu_;
};
}  // namespace hf3fs::mgmtd
