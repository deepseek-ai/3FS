#pragma once

#include <cassert>
#include <fmt/core.h>
#include <folly/Executor.h>
#include <folly/Synchronized.h>
#include <folly/functional/Invoke.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "client/mgmtd/IMgmtdClientForServer.h"
#include "common/app/ClientId.h"
#include "common/app/NodeId.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/Serde.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/String.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "meta/store/FileSession.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

class FileHelper;

class SessionManager {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(enable, true);
    CONFIG_HOT_UPDATED_ITEM(scan_interval, 5_min);
    CONFIG_HOT_UPDATED_ITEM(scan_batch, 1024u);
    CONFIG_HOT_UPDATED_ITEM(sync_on_prune_session, false);
    CONFIG_HOT_UPDATED_ITEM(session_timeout, 5_min);
    CONFIG_OBJ(scan_workers, CoroutinesPoolBase::Config, [](auto &c) {
      c.set_coroutines_num(8);
      c.set_queue_size(128);
    });
    CONFIG_OBJ(close_workers, CoroutinesPoolBase::Config, [](auto &c) {
      c.set_coroutines_num(32);
      c.set_queue_size(1024);
    });
  };

  SessionManager(const Config &cfg,
                 flat::NodeId nodeId,
                 std::shared_ptr<kv::IKVEngine> kvEngine,
                 std::shared_ptr<client::ICommonMgmtdClient> mgmtd,
                 std::shared_ptr<FileHelper> fileHelper)
      : config_(cfg),
        nodeId_(nodeId),
        kvEngine_(kvEngine),
        mgmtd_(mgmtd),
        fileHelper_(fileHelper) {}
  ~SessionManager() { stopAndJoin(); }

  void start(CPUExecutorGroup &exec);
  void stopAndJoin();

  using CloseFunc = std::function<CoTryTask<void>(const meta::CloseReq &)>;
  void setCloseFunc(CloseFunc close) { close_ = close; }

  // for admin_cli
  CoTryTask<std::vector<FileSession>> listSessions();
  CoTryTask<std::vector<FileSession>> listSessions(InodeId inodeId);
  CoTryTask<size_t> pruneManually();

 private:
  struct PruneSessions {
    std::atomic<size_t> finished = 0;
    folly::Synchronized<std::map<Uuid, FileSession>> sessions;
  };

  class ScanTask {
   public:
    ScanTask(size_t shard, std::shared_ptr<PruneSessions> prune)
        : shard_(shard),
          prune_(prune) {}

    CoTryTask<size_t> run(SessionManager &manager);

   private:
    size_t shard_ = -1;
    std::shared_ptr<PruneSessions> prune_;
  };

  // try to close and sync
  class CloseTask {
   public:
    CloseTask(FileSession session)
        : session_(std::move(session)) {}
    CoTryTask<void> run(SessionManager &manager);

   private:
    FileSession session_;
  };

  CoTask<void> scanTask();
  CoTryTask<std::shared_ptr<PruneSessions>> loadPrune();

  const Config &config_;
  flat::NodeId nodeId_;
  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::shared_ptr<client::ICommonMgmtdClient> mgmtd_;
  std::shared_ptr<FileHelper> fileHelper_;
  std::unique_ptr<BackgroundRunner> scanRunner_;
  std::unique_ptr<CoroutinesPool<std::unique_ptr<ScanTask>>> scanWorkers_;
  std::unique_ptr<CoroutinesPool<std::unique_ptr<CloseTask>>> closeWorkers_;
  CloseFunc close_;
};

}  // namespace hf3fs::meta::server

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::meta::server::FileSession> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::meta::server::FileSession &session, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(),
                          "{{inodeId {}, client {}, session {}}}",
                          session.inodeId,
                          session.clientId,
                          session.sessionId);
  }
};

FMT_END_NAMESPACE