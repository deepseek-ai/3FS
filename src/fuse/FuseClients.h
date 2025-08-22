#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <folly/MPMCQueue.h>
#include <folly/Math.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>

#include "common/utils/BackgroundRunner.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Result.h"
#include "common/utils/Semaphore.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#define FUSE_USE_VERSION 312
#define OP_LOG_LEVEL DBG

#include <folly/concurrency/AtomicSharedPtr.h>
#include <fuse3/fuse_lowlevel.h>

#include "FuseConfig.h"
#include "IoRing.h"
#include "IovTable.h"
#include "PioV.h"
#include "UserConfig.h"
#include "client/meta/MetaClient.h"
#include "client/mgmtd/MgmtdClientForClient.h"
#include "client/storage/StorageClient.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::fuse {
using flat::Gid;
using flat::Uid;
using flat::UserInfo;
using lib::agent::PioV;
using meta::Acl;
using meta::Directory;
using meta::DirEntry;
using meta::Inode;
using meta::InodeData;
using meta::InodeId;
using meta::Permission;
using storage::client::IOBuffer;

struct InodeWriteBuf {
  std::vector<uint8_t> buf;
  std::unique_ptr<storage::client::IOBuffer> memh;
  off_t off{0};
  size_t len{0};
};

struct RcInode {
  struct DynamicAttr {
    uint64_t written = 0;
    uint64_t synced = 0;   // period sync
    uint64_t fsynced = 0;  // fsync, close, truncate, etc...
    flat::Uid writer = flat::Uid(0);

    uint32_t dynStripe = 1;  // dynamic stripe

    uint64_t truncateVer = 0;                         // largest known truncate version.
    std::optional<meta::VersionedLength> hintLength;  // local hint length
    std::optional<UtcTime> atime;                     // local read time, but only update for write open
    std::optional<UtcTime> mtime;                     // local write time

    void update(const Inode &inode, uint64_t syncver = 0, bool fsync = false) {
      if (!inode.isFile()) {
        return;
      }

      synced = std::max(synced, syncver);
      if (written == synced) {
        // clear local hint, since not write happens after sync
        hintLength = meta::VersionedLength{0, 0};
      }
      if (fsync) {
        fsynced = std::max(fsynced, syncver);
      }
      truncateVer = std::max(truncateVer, inode.asFile().truncateVer);
      dynStripe = inode.asFile().dynStripe;
    }
  };

  Inode inode;
  int refcount;
  std::atomic<int> opened;

  std::mutex wbMtx;
  std::shared_ptr<InodeWriteBuf> writeBuf;

  folly::Synchronized<DynamicAttr> dynamicAttr;
  folly::coro::Mutex extendStripeLock;

  RcInode(Inode inode, int refcount = 1)
      : inode(inode),
        refcount(refcount),
        extendStripeLock() {
    if (inode.isFile()) {
      auto guard = dynamicAttr.wlock();
      guard->truncateVer = inode.asFile().truncateVer;
      guard->hintLength = meta::VersionedLength{0, guard->truncateVer};
      guard->dynStripe = inode.asFile().dynStripe;
    }
  }

  uint64_t getTruncateVer() const { return dynamicAttr.rlock()->truncateVer; }

  void update(const Inode &inode, uint64_t syncver = 0, bool fsync = false) {
    if (!inode.isFile()) {
      return;
    } else {
      auto guard = dynamicAttr.wlock();
      return guard->update(inode, syncver, fsync);
    }
  }

  // clear hint length, force calculate length on next sync
  void clearHintLength() {
    auto guard = dynamicAttr.wlock();
    guard->hintLength = std::nullopt;
  }

  CoTryTask<uint64_t> beginWrite(flat::UserInfo userInfo,
                                 meta::client::MetaClient &meta,
                                 uint64_t offset,
                                 uint64_t length);

  void finishWrite(flat::UserInfo userInfo, uint64_t truncateVer, uint64_t offset, ssize_t ret);
};

struct FileHandle {
  std::shared_ptr<RcInode> rcinode;
  bool oDirect;
  Uuid sessionId;

  /* FileHandle(std::shared_ptr<RcInode> rcinode, bool oDirect, Uuid sessionId) */
  /*       : rcinode(rcinode), */
  /*         sessionId(sessionId) {} */
};

struct DirHandle {
  size_t dirId;
  pid_t pid;
  bool iovDir;
};

struct DirEntryVector {
  std::shared_ptr<std::vector<DirEntry>> dirEntries;

  DirEntryVector(std::shared_ptr<std::vector<DirEntry>> &&dirEntries)
      : dirEntries(std::move(dirEntries)) {}
};

struct DirEntryInodeVector {
  std::shared_ptr<std::vector<DirEntry>> dirEntries;
  std::shared_ptr<std::vector<std::optional<Inode>>> inodes;

  DirEntryInodeVector(std::shared_ptr<std::vector<DirEntry>> dirEntries,
                      std::shared_ptr<std::vector<std::optional<Inode>>> inodes)
      : dirEntries(std::move(dirEntries)),
        inodes(std::move(inodes)) {}
};

struct FuseClients {
  FuseClients() = default;
  ~FuseClients();

  Result<Void> init(const flat::AppInfo &appInfo,
                    const String &mountPoint,
                    const String &tokenFile,
                    FuseConfig &fuseConfig);
  void stop();

  CoTask<void> ioRingWorker(int i, int ths);
  void watch(int prio, std::stop_token stop);

  CoTask<void> periodicSyncScan();
  CoTask<void> periodicSync(InodeId inodeId);

  std::unique_ptr<net::Client> client;
  std::shared_ptr<client::MgmtdClientForClient> mgmtdClient;
  std::shared_ptr<storage::client::StorageClient> storageClient;
  std::shared_ptr<meta::client::MetaClient> metaClient;

  std::string fuseToken;
  std::string fuseMount;
  Path fuseMountpoint;
  std::optional<Path> fuseRemountPref;
  std::atomic<bool> memsetBeforeRead = false;
  int maxIdleThreads = 0;
  int maxThreads = 0;
  bool enableWritebackCache = false;

  std::unique_ptr<ConfigCallbackGuard> onFuseConfigUpdated;

  std::unordered_map<InodeId, std::shared_ptr<RcInode>> inodes = {
      {InodeId::root(), std::make_shared<RcInode>(Inode{}, 2)}};
  std::mutex inodesMutex;

  std::unordered_map<uint64_t, DirEntryInodeVector> readdirplusResults;
  std::mutex readdirplusResultsMutex;

  std::atomic_uint64_t dirHandle{0};

  std::shared_ptr<net::RDMABufPool> bufPool;
  int maxBufsize = 0;

  fuse_session *se = nullptr;

  std::atomic<std::chrono::nanoseconds> jitter;

  IovTable iovs;
  IoRingTable iors;
  std::vector<std::unique_ptr<BoundedQueue<IoRingJob>>> iojqs;  // job queues
  std::vector<std::jthread> ioWatches;
  folly::CancellationSource cancelIos;

  UserConfig userConfig;

  folly::Synchronized<std::set<InodeId>, std::mutex> dirtyInodes;
  std::atomic<InodeId> lastSynced;
  std::unique_ptr<BackgroundRunner> periodicSyncRunner;
  std::unique_ptr<CoroutinesPool<InodeId>> periodicSyncWorker;

  std::unique_ptr<folly::IOThreadPoolExecutor> notifyInvalExec;
  const FuseConfig *config;
};
}  // namespace hf3fs::fuse
