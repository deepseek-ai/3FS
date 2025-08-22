#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <folly/Executor.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest_prod.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/MgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/app/ApplicationBase.h"
#include "common/app/NodeId.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/CountDownLatch.h"
#include "common/utils/Duration.h"
#include "common/utils/PriorityCoroutinePool.h"
#include "common/utils/Result.h"
#include "common/utils/Semaphore.h"
#include "common/utils/UtcTime.h"
#include "core/user/UserStoreEx.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fdb/FDBRetryStrategy.h"
#include "fmt/core.h"
#include "meta/base/Config.h"
#include "meta/components/InodeIdAllocator.h"
#include "meta/components/SessionManager.h"
#include "meta/event/Event.h"
#include "meta/store/DirEntry.h"
#include "meta/store/FileSession.h"
#include "scn/scan/scan.h"

namespace hf3fs::meta::server {

class FileHelper;
using hf3fs::client::ICommonMgmtdClient;

class GcManager {
 public:
  static Result<std::pair<UtcTime, InodeId>> parseGcEntry(std::string_view entry) {
    char prefix;
    uint64_t timestamp;
    uint64_t inode;
    auto ret = scn::scan(entry, "{}-{}-{:i}", prefix, timestamp, inode);
    if (!ret) {
      return makeError(StatusCode::kInvalidArg);
    }
    return std::pair<UtcTime, InodeId>{UtcTime::fromMicroseconds(timestamp), InodeId(inode)};
  }

  static std::string formatGcEntry(char prefix, UtcTime timestamp, InodeId inode) {
    return fmt::format("{}-{:020d}-{}", prefix, (uint64_t)timestamp.toMicroseconds(), inode.toHexString());
  }

  GcManager(const Config &config,
            flat::NodeId nodeId,
            analytics::StructuredTraceLog<MetaEventTrace> &metaEventTraceLog,
            std::shared_ptr<kv::IKVEngine> kvEngine,
            std::shared_ptr<ICommonMgmtdClient> mgmtd,
            std::shared_ptr<InodeIdAllocator> idAlloc,
            std::shared_ptr<FileHelper> fileHelper,
            std::shared_ptr<SessionManager> sessionManager,
            std::shared_ptr<core::UserStoreEx> userStore)
      : config_(config),
        nodeId_(nodeId),
        metaEventTraceLog_(metaEventTraceLog),
        kvEngine_(kvEngine),
        mgmtd_(mgmtd),
        idAlloc_(idAlloc),
        fileHelper_(fileHelper),
        sessionManager_(sessionManager),
        userStore_(userStore),
        concurrentGcDirSemaphore_(config_.gc().gc_directory_concurrent()),
        concurrentGcFileSemaphore_(config_.gc().gc_file_concurrent()) {
    XLOGF_IF(FATAL, !nodeId_, "invalid node id {}", nodeId_);
    guard_ = config_.gc().addCallbackGuard([&]() {
      auto dirConcurrent = config_.gc().gc_directory_concurrent();
      if (dirConcurrent != 0 && dirConcurrent != concurrentGcDirSemaphore_.getUsableTokens()) {
        XLOGF(INFO, "GcManager set gc directory concurrent to {}", dirConcurrent);
        concurrentGcDirSemaphore_.changeUsableTokens(dirConcurrent);
        XLOGF(INFO, "GcManager finished update gc directory concurrent");
      }
      auto fileConcurrent = config_.gc().gc_file_concurrent();
      if (fileConcurrent != 0 && fileConcurrent != concurrentGcFileSemaphore_.getUsableTokens()) {
        XLOGF(INFO, "GcManager set gc directory concurrent to {}", fileConcurrent);
        concurrentGcFileSemaphore_.changeUsableTokens(fileConcurrent);
        XLOGF(INFO, "GcManager finished update gc file concurrent");
      }
    });
  }

  CoTryTask<void> init();

  void start(CPUExecutorGroup &exec);
  void stopAndJoin();

  auto &getEventTraceLog() { return metaEventTraceLog_; }

  CoTryTask<void> removeEntry(IReadWriteTransaction &txn, const DirEntry &entry, Inode &inode, GcInfo gcInfo);

 private:
  template <typename T>
  FRIEND_TEST(TestRemove, GC);

  enum GcEntryType {
    DIRECTORY = 0,
    FILE_MEDIUM,
    FILE_LARGE,
    FILE_SMALL,
    MAX,
  };

  class GcDirectory;

  struct GcTask {
    std::shared_ptr<GcDirectory> gcDir;
    GcEntryType type;
    DirEntry taskEntry;

    GcTask(std::shared_ptr<GcDirectory> gcDir, GcEntryType type, DirEntry entry)
        : gcDir(std::move(gcDir)),
          type(type),
          taskEntry(std::move(entry)) {}

    static CoTryTask<flat::UserAttr> getUserAttr(GcManager &manager, flat::Uid uid);

    CoTryTask<void> run(GcManager &manager);
    CoTryTask<void> gcDirectory(GcManager &manager);
    CoTryTask<void> gcFile(GcManager &manager);

    CoTryTask<void> removeEntry(GcManager &manager,
                                IReadWriteTransaction &txn,
                                const DirEntry &entry,
                                const flat::UserAttr &user);
    CoTryTask<void> removeGcEntryAndInode(GcManager &manager, IReadWriteTransaction &txn);
    CoTryTask<DirEntry> createOrphanEntry(GcManager &manager,
                                          IReadWriteTransaction &txn,
                                          const DirEntry &entry,
                                          const Inode &inode,
                                          const flat::UserAttr &user);
  };

  class GcDirectory : folly::MoveOnly, public std::enable_shared_from_this<GcDirectory> {
   public:
    using Ptr = std::shared_ptr<GcDirectory>;

    static char prefixOf(GcEntryType type) {
      switch (type) {
        case DIRECTORY:
          return 'd';
        case FILE_MEDIUM:
          return 'f';
        case FILE_LARGE:
          return 'L';
        case FILE_SMALL:
          return 'S';
        default:
          XLOGF(FATAL, "invalid type {}", (int)type);
      }
    }

    int8_t priorityOf(GcEntryType type) {
      switch (type) {
        case DIRECTORY:
          return folly::Executor::MID_PRI;
        case FILE_MEDIUM:
          return folly::Executor::MID_PRI;
        case FILE_LARGE:
          return folly::Executor::HI_PRI;
        case FILE_SMALL:
          return folly::Executor::LO_PRI;
        default:
          XLOGF(FATAL, "invalid type {}", (int)type);
      }
    }

    static std::string nameOf(flat::NodeId nodeId, size_t idx) {
      return idx == 0 ? fmt::format("GC-Node-{}", (uint32_t)nodeId)
                      : fmt::format("GC-Node-{}.{}", (uint32_t)nodeId, idx);
    }

    GcDirectory(DirEntry entry)
        : entry_(std::move(entry)) {}
    ~GcDirectory() { stopAndJoin(); }

    void start(GcManager &manager, CPUExecutorGroup &exec);
    void stopAndJoin();

    auto dirId() const { return entry_.id; }
    std::string name() const { return entry_.name; }

    CoTryTask<void> add(auto &txn, const Inode &inode, const GcConfig &config, GcInfo gcInfo);
    void finish(const GcTask &task);

    CoTryTask<void> moveToTail(auto &txn, const GcTask &task, Duration delay);

   private:
    struct QueueState {
      folly::Synchronized<std::set<InodeId>, std::mutex> queued;
      folly::Synchronized<std::set<InodeId>, std::mutex> finished;
      std::atomic<uint64_t> counter{0};
    };

    CoTask<void> scan(GcManager &manager, GcEntryType type);
    CoTryTask<bool> scan(GcManager &manager,
                         GcEntryType type,
                         Duration delay,
                         size_t limit,
                         std::optional<std::string> &prev);
    CoTryTask<void> addFile(auto &txn, const Inode &inode, const GcConfig &config);
    CoTryTask<void> addDirectory(auto &txn, const Inode &inode, GcInfo gcInfo);

    DirEntry entry_;  // entry points to this GcDirectory
    std::array<QueueState, GcEntryType::MAX> states_;
    CancellationSource cancel_;
    CountDownLatch<> latch_;
  };

  const std::vector<GcDirectory::Ptr> &currGcDirectories() const { return currGcDirectories_; }

  GcDirectory::Ptr pickGcDirectory() {
    XLOGF_IF(FATAL, currGcDirectories_.empty(), "currGcDirectories_.empty()");
    if (config_.gc().distributed_gc()) {
      auto guard = allGcDirectories_.rlock();
      if (!guard->empty()) {
        return guard->at(folly::Random::rand64(guard->size()));
      }
    }
    if (currGcDirectories_.size() == 1) {
      return currGcDirectories_[0];
    } else {
      return currGcDirectories_[folly::Random::rand32(1, currGcDirectories_.size())];
    }
  }

  bool enableGcDelay() const;

  CoTryTask<void> checkFs();
  CoTryTask<std::shared_ptr<GcDirectory>> openGcDirectory(size_t idx, bool create);
  CoTryTask<void> scanAllGcDirectories();

  CoTask<void> runGcTask(GcTask task);

  template <typename H>
  std::invoke_result_t<H, IReadOnlyTransaction &> runReadOnly(H &&handler) {
    auto retry = kv::FDBRetryStrategy({1_s, 10, true});
    co_return co_await kv::WithTransaction(retry).run(kvEngine_->createReadonlyTransaction(), std::forward<H>(handler));
  }

  template <typename H>
  std::invoke_result_t<H, IReadWriteTransaction &> runReadWrite(H &&handler) {
    auto retry = kv::FDBRetryStrategy({1_s, 10, true});
    co_return co_await kv::WithTransaction(retry).run(kvEngine_->createReadWriteTransaction(),
                                                      std::forward<H>(handler));
  }

  const Config &config_;
  std::unique_ptr<config::ConfigCallbackGuard> guard_;
  flat::NodeId nodeId_;
  analytics::StructuredTraceLog<MetaEventTrace> &metaEventTraceLog_;
  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::shared_ptr<ICommonMgmtdClient> mgmtd_;
  std::shared_ptr<InodeIdAllocator> idAlloc_;
  std::shared_ptr<FileHelper> fileHelper_;
  std::shared_ptr<SessionManager> sessionManager_;
  std::shared_ptr<core::UserStoreEx> userStore_;

  std::vector<GcDirectory::Ptr> currGcDirectories_;
  folly::Synchronized<std::vector<GcDirectory::Ptr>> allGcDirectories_;

  std::unique_ptr<BackgroundRunner> gcRunner_;
  std::unique_ptr<PriorityCoroutinePool<GcTask>> gcWorkers_;
  Semaphore concurrentGcDirSemaphore_;
  Semaphore concurrentGcFileSemaphore_;
};

}  // namespace hf3fs::meta::server
