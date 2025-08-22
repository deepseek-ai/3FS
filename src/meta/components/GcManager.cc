#include "meta/components/GcManager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <linux/fs.h>
#include <linux/limits.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/app/NodeId.h"
#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/monitor/Recorder.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Coroutine.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/SemaphoreGuard.h"
#include "common/utils/StatusCode.h"
#include "common/utils/SysResource.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fdb/FDBTransaction.h"
#include "fmt/format.h"
#include "foundationdb/fdb_c_types.h"
#include "meta/components/FileHelper.h"
#include "meta/event/Event.h"
#include "meta/store/DirEntry.h"
#include "meta/store/FileSession.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"
#include "meta/store/ops/SetAttr.h"

namespace hf3fs::meta::server {

static constexpr size_t kNumGcDirectoryPerServer = 5;  // 4 + 1

namespace {
monitor::CountRecorder gcSuccCount("meta_server.gc_success");
monitor::CountRecorder gcFailCount("meta_server.gc_fail");
monitor::CountRecorder gcCritical("meta_server.gc_critical");
monitor::CountRecorder gcEnqueue("meta_server.gc_enqueue");
monitor::CountRecorder gcBusy("meta_server.gc_busy");
monitor::LatencyRecorder gcLatency("meta_server.gc_latency");
monitor::DistributionRecorder chunksDist("meta_server.gc_chunks");
}  // namespace

/* GcManager::GcDirectory */
void GcManager::GcDirectory::start(GcManager &manager, CPUExecutorGroup &exec) {
  std::vector<GcEntryType> types{GcEntryType::DIRECTORY,
                                 GcEntryType::FILE_LARGE,
                                 GcEntryType::FILE_MEDIUM,
                                 GcEntryType::FILE_SMALL};
  assert(types.size() == GcEntryType::MAX);
  for (auto type : types) {
    latch_.increase();
    co_withCancellation(cancel_.getToken(), scan(manager, type)).scheduleOn(&exec.pickNext()).start();
  }
}

void GcManager::GcDirectory::stopAndJoin() {
  cancel_.requestCancellation();
  folly::coro::blockingWait(latch_.wait());
}

CoTask<void> GcManager::GcDirectory::scan(GcManager &manager, GcEntryType type) {
  SCOPE_EXIT { latch_.countDown(); };

  std::optional<std::string> prev;
  auto prevTime = SteadyClock::now();
  while (true) {
    auto wait = manager.config_.gc().scan_interval();
    if (manager.config_.gc().enable()) {
      if (auto now = SteadyClock::now(); now - prevTime > 5_s) {
        prev = std::nullopt;
        prevTime = now;
      }
      auto delay = type == GcEntryType::DIRECTORY ? manager.config_.gc().gc_directory_delay()
                                                  : manager.config_.gc().gc_file_delay();
      auto result =
          co_await scan(manager, type, manager.enableGcDelay() ? delay : 0_ms, manager.config_.gc().scan_batch(), prev);
      if (!result) {
        XLOGF(ERR, "GcDirectory {} scan {} failed, error {}", name(), magic_enum::enum_name(type), result.error());
      } else if (!*result) {
        wait = std::max(wait, 100_ms);
      }
    }
    auto res = co_await folly::coro::co_awaitTry(folly::coro::sleep(wait.asUs()));
    if (UNLIKELY(res.hasException())) {
      XLOGF_IF(FATAL, !res.hasException<OperationCancelled>(), "Exception {}", res.exception().what());
      XLOGF(INFO, "GcDirectory::scan {} {} exit", entry_.name, magic_enum::enum_name(type));
      break;
    }
  }
}

CoTryTask<bool> GcManager::GcDirectory::scan(GcManager &manager,
                                             GcEntryType type,
                                             Duration delay,
                                             size_t limit,
                                             std::optional<std::string> &prev) {
  auto &state = states_[type];
  XLOGF(DBG, "GcDirectory {} scan {}, queued {}", name(), magic_enum::enum_name(type), state.counter.load());

  auto prefix = prefixOf(type);
  std::string prefixstr = fmt::format("{}", prefix);
  std::string beginkey = prefixstr;
  auto endtime = UtcTime::fromMicroseconds(UtcClock::now().toMicroseconds() - delay.asUs().count());
  std::string endkey = formatGcEntry(prefix, endtime, InodeId::root());
  XLOGF_IF(FATAL, beginkey >= endkey, "{} >= {}", beginkey, endkey);

  // skip some keys
  if (prev && *prev > beginkey) {
    beginkey = std::min(*prev, endkey);
  }

  auto queued = state.queued.lock();
  auto empty = true;
  while (beginkey < endkey) {
    auto cnt = state.counter.load();
    if (cnt >= limit) {
      XLOGF(DBG, "GcDirectory skip scan, queuedCnt {}", cnt);
      co_return true;
    }

    auto finished = state.finished.withLock([](auto &v) { return std::exchange(v, {}); });
    for (auto &inode : finished) {
      queued->erase(inode);
    }

    auto txn = manager.kvEngine_->createReadonlyTransaction();
    auto result = co_await DirEntryList::snapshotLoad(*txn, dirId(), beginkey, endkey, limit - cnt);
    CO_RETURN_ON_ERROR(result);
    for (auto &entry : result->entries) {
      if (!queued->contains(entry.id)) {
        queued->insert(entry.id);
        state.counter++;
        auto task = GcTask(shared_from_this(), type, entry);
        manager.gcWorkers_->enqueue(std::move(task), priorityOf(type));
      }
    }

    if (!result->entries.empty()) {
      beginkey = result->entries.back().name;
      prev = result->entries.back().name;
      empty = false;
    }
    if (!result->more) {
      break;
    }
  }

  co_return !empty;
}

void GcManager::GcDirectory::finish(const GcManager::GcTask &task) {
  XLOGF_IF(FATAL, task.type >= GcEntryType::MAX, "Invalid GcEntryType {}", (int)task.type);
  auto &state = states_[task.type];
  state.finished.lock()->insert(task.taskEntry.id);
  auto cnt = state.counter--;
  XLOGF_IF(FATAL, cnt < 0, "cnt {}", cnt);
}

CoTryTask<void> GcManager::GcDirectory::add(auto &txn, const Inode &inode, const GcConfig &config, GcInfo gcInfo) {
  gcEnqueue.addSample(1);
  switch (inode.getType()) {
    case InodeType::File:
      co_return co_await addFile(txn, inode, config);
    case InodeType::Directory:
      // only directory need gcInfo
      co_return co_await addDirectory(txn, inode, gcInfo);
    default:
      XLOGF(FATAL, "Invalid inode type {}, inode {}", magic_enum::enum_name(inode.getType()), inode);
  }
}

CoTryTask<void> GcManager::GcDirectory::addFile(auto &txn, const Inode &inode, const GcConfig &config) {
  auto prefix = prefixOf(GcEntryType::FILE_MEDIUM);
  auto chunks = inode.asFile().length / inode.asFile().layout.chunkSize;
  if (chunks >= config.large_file_chunks()) {
    prefix = prefixOf(GcEntryType::FILE_LARGE);
  }
  if (chunks < config.small_file_chunks()) {
    prefix = prefixOf(GcEntryType::FILE_SMALL);
  }
  auto entry = DirEntry::newFile(dirId(), formatGcEntry(prefix, UtcClock::now(), inode.id), inode.id);
  CO_RETURN_ON_ERROR(co_await entry.store(txn));
  XLOGF(DBG, "GcManager create GC entry {}", entry);
  co_return Void{};
}

CoTryTask<void> GcManager::GcDirectory::addDirectory(auto &txn, const Inode &inode, GcInfo gcInfo) {
  auto prefix = prefixOf(GcEntryType::DIRECTORY);
  auto entry = DirEntry::newDirectory(dirId(), formatGcEntry(prefix, UtcClock::now(), inode.id), inode.id, inode.acl);
  entry.gcInfo = gcInfo;
  CO_RETURN_ON_ERROR(co_await entry.store(txn));
  XLOGF(DBG, "GcManager create GC entry {}", entry);
  co_return Void{};
}

CoTryTask<void> GcManager::GcDirectory::moveToTail(auto &txn, const GcTask &task, Duration delay) {
  // check task entry still exists
  auto exists = co_await DirEntry::checkExist(txn, task.taskEntry.parent, task.taskEntry.name);
  CO_RETURN_ON_ERROR(exists);
  if (*exists) {
    auto entry = task.taskEntry;
    entry.name = formatGcEntry(prefixOf(task.type), UtcClock::now() + delay.asUs(), task.taskEntry.id);
    XLOGF_IF(FATAL, entry.isSymlink(), "entry is symlink {}", entry);
    XLOGF_IF(FATAL, !entry.valid(), "Invalid entry {} {}", entry, entry.valid().error());
    CO_RETURN_ON_ERROR(co_await entry.store(txn));
    CO_RETURN_ON_ERROR(co_await task.taskEntry.remove(txn));
  }
  co_return Void{};
}

/* GcManager::GcTask */
CoTryTask<flat::UserAttr> GcManager::GcTask::getUserAttr(GcManager &manager, flat::Uid uid) {
  auto res = co_await manager.userStore_->getUser(uid);
  if (res.hasError() && res.error().code() == StatusCode::kAuthenticationFail) {
    // user not found, maybe running in unittest, or user has been removed
    auto user = flat::UserAttr();
    user.uid = uid;
    user.gid = flat::Gid(uid.toUnderType());
    user.groups = {};
    user.name = fmt::format("user-{}", uid.toUnderType());
    co_return user;
  }
  co_return res;
}

CoTryTask<void> GcManager::GcTask::run(GcManager &manager) {
  XLOGF(DBG, "GcTask {} run", taskEntry);
  Result<Void> result = makeError(MetaCode::kFoundBug);
  switch (taskEntry.type) {
    case InodeType::File:
      result = co_await gcFile(manager);
      break;
    case InodeType::Directory:
      result = co_await gcDirectory(manager);
      break;
    case InodeType::Symlink:
    default:
      XLOGF(FATAL, "Invalid type {}, {}", magic_enum::enum_name(taskEntry.type), taskEntry);
  }

  if (!result.hasError()) {
    co_return Void{};
  }

  auto code = result.error().code();
  auto critical = (StatusCode::typeOf(code) == StatusCodeType::Meta && code != MetaCode::kBusy)  // Meta error code
                  || (code == StorageClientCode::kInvalidArg || code == StorageClientCode::kChecksumMismatch ||
                      code == StorageClientCode::kFoundBug);  // storage error code
  if (critical || code == MetaCode::kBusy) {
    if (critical) {
      XLOGF(CRITICAL, "GcTask {} run failed, error {}", taskEntry.id, result.error());
      gcCritical.addSample(1);
    }
    co_await manager.runReadWrite([&](IReadWriteTransaction &txn) -> CoTryTask<void> {
      co_return co_await gcDir->moveToTail(txn, *this, manager.config_.gc().retry_delay());
    });
  }
  if (code == StorageClientCode::kReadOnlyServer) {
    XLOGF(ERR, "GcTask {} run failed, readonly server, {}", taskEntry, result.error());
    co_await folly::coro::sleep(std::chrono::seconds(1));
  }
  co_return result;
}

CoTryTask<void> GcManager::GcTask::gcDirectory(GcManager &manager) {
  SemaphoreGuard guard(manager.concurrentGcDirSemaphore_);
  co_await guard.coWait();

  XLOGF_IF(DFATAL, !taskEntry.isDirectory(), "{} is not directory", taskEntry);

  // old version did not record who performed the recursive remove
  // just assume that the directory owner performed this operation.
  auto uid = taskEntry.gcInfo ? taskEntry.gcInfo->user : taskEntry.dirAcl->uid;
  auto user = co_await getUserAttr(manager, uid);
  CO_RETURN_ON_ERROR(user);

  auto finished = false;
  auto checked = false;
  auto handler = [&](IReadWriteTransaction &txn) -> CoTryTask<void> {
    auto fdbTxn = dynamic_cast<kv::FDBTransaction *>(&txn);
    if (fdbTxn && manager.config_.gc().txn_low_priority()) {
      fdbTxn->setOption(FDBTransactionOption::FDB_TR_OPTION_PRIORITY_BATCH, {});
    }
    if (!checked) {
      auto loadInode = co_await Inode::load(txn, taskEntry.id);
      CO_RETURN_ON_ERROR(loadInode);
      if (!loadInode->has_value()) {
        // inode is already removed, may happens when retry transaction
        XLOGF(ERR, "taskEntry {}, inode already removed", taskEntry);
        CO_RETURN_ON_ERROR(co_await removeGcEntryAndInode(manager, txn));
        finished = true;
        co_return Void{};
      }
      // sanity check
      auto &inode = **loadInode;
      if (inode.nlink || !inode.isDirectory() || inode.acl.iflags & FS_IMMUTABLE_FL) {
        XLOGF(DFATAL,
              "taskEntry {}, inode {}, nlink {}, directory {}, immutable {}",
              taskEntry,
              inode,
              inode.nlink,
              inode.isDirectory(),
              inode.acl.iflags & FS_IMMUTABLE_FL);
        co_return makeError(MetaCode::kFoundBug);
      }

      Event(Event::Type::GC)
          .addField("inode", inode.id)
          .addField("owner", inode.acl.uid)
          .addField("parent", inode.asDirectory().parent)
          .addField("name", inode.asDirectory().name)
          .log();
      manager.getEventTraceLog().newEntry(MetaEventTrace{.eventType = Event::Type::GC,
                                                         .inodeId = inode.id,
                                                         .parentId = inode.asDirectory().parent,
                                                         .entryName = inode.asDirectory().name,
                                                         .ownerId = inode.acl.uid});
      checked = true;
    }

    auto list = co_await DirEntryList::load(txn, taskEntry.id, "", manager.config_.gc().gc_directory_entry_batch());
    CO_RETURN_ON_ERROR(list);

    for (auto &entry : list->entries) {
      CO_RETURN_ON_ERROR(co_await removeEntry(manager, txn, entry, *user));
    }

    if (!list->more) {
      CO_RETURN_ON_ERROR(co_await removeGcEntryAndInode(manager, txn));
      finished = true;
    }
    co_return Void{};
  };

  while (!finished) {
    CO_RETURN_ON_ERROR(co_await manager.runReadWrite(handler));
  }

  co_return Void{};
}

CoTryTask<void> GcManager::GcTask::gcFile(GcManager &manager) {
  SemaphoreGuard guard(manager.concurrentGcFileSemaphore_);
  co_await guard.coWait();
  assert(taskEntry.isFile());
  XLOGF(DBG, "Gc file {}", taskEntry.id);

  auto load = co_await manager.runReadOnly([&](auto &txn) -> CoTryTask<std::optional<Inode>> {
    auto fdbTxn = dynamic_cast<kv::FDBTransaction *>(&txn);
    if (fdbTxn && manager.config_.gc().txn_low_priority()) {
      fdbTxn->setOption(FDBTransactionOption::FDB_TR_OPTION_PRIORITY_BATCH, {});
    }
    if (!manager.config_.gc().check_session()) {
      co_return co_await Inode::snapshotLoad(txn, taskEntry.id);
    }

    auto [inode, session] = co_await folly::coro::collectAll(Inode::snapshotLoad(txn, taskEntry.id),
                                                             FileSession::snapshotCheckExists(txn, taskEntry.id));
    CO_RETURN_ON_ERROR(inode);
    CO_RETURN_ON_ERROR(session);
    if (*session) {
      XLOGF(CRITICAL, "Delay gc file {}, still has session {}.", taskEntry.id, session.value()->clientId);
      gcBusy.addSample(1);
      co_return makeError(MetaCode::kBusy, "still have session");
    }
    // sanity check
    if (inode->has_value()) {
      if (inode->value().nlink || inode->value().acl.iflags & FS_IMMUTABLE_FL) {
        XLOGF(DFATAL,
              "taskEntry {}, inode {}, nlink {}, immutable {}",
              taskEntry,
              **inode,
              inode->value().nlink,
              inode->value().acl.iflags & FS_IMMUTABLE_FL);
        co_return makeError(MetaCode::kFoundBug);
      }
    }
    co_return inode;
  });
  CO_RETURN_ON_ERROR(load);
  auto &inode = *load;
  std::optional<Event> event;
  std::optional<MetaEventTrace> trace;
  if (LIKELY(inode.has_value())) {
    auto result = co_await manager.fileHelper_->remove(UserInfo(Uid(0), Gid(0)),
                                                       *inode,
                                                       manager.config_.gc().retry_remove_chunks(),
                                                       manager.config_.gc().remove_chunks_batch_size());
    if (result.hasError()) {
      XLOGF(ERR, "GcManager failed to remove chunks for {}, error {}", inode->id, result.error());
      CO_RETURN_ON_ERROR(result);
    }
    auto chunks = *result;
    XLOGF(DBG, "GcManager removed {} chunks for {}", chunks, inode->id);
    chunksDist.addSample(chunks);
    event = Event(Event::Type::GC)
                .addField("inode", inode->id)
                .addField("owner", inode->acl.uid)
                .addField("length", inode->asFile().length)
                .addField("chunks", chunks);
    trace = MetaEventTrace{
        .eventType = Event::Type::GC,
        .inodeId = inode->id,
        .ownerId = inode->acl.uid,
        .length = inode->asFile().length,
        .removedChunks = chunks,
    };
  } else {
    XLOGF(CRITICAL, "Inode of {} not found, shouldn't happen!!", taskEntry.id);
  }

  auto remove =
      co_await manager.runReadWrite([&](IReadWriteTransaction &txn) { return removeGcEntryAndInode(manager, txn); });
  if (remove.hasError()) {
    XLOGF(ERR, "GcManager failed to remove GC entry and Inode for {}, error {}", taskEntry.id, remove.error());
    CO_RETURN_ON_ERROR(remove);
  }

  if (event) {
    event->log();
  }

  if (trace) {
    manager.getEventTraceLog().append(*trace);
  }

  co_return Void{};
}

CoTryTask<void> GcManager::GcTask::removeEntry(GcManager &manager,
                                               IReadWriteTransaction &txn,
                                               const DirEntry &entry,
                                               const flat::UserAttr &user) {
  if (entry.parent != taskEntry.id) {
    XLOGF(DFATAL, "{}.parent != {}.id", entry, taskEntry);
    co_return makeError(MetaCode::kFoundBug);
  }
  auto inode = co_await entry.loadInode(txn);
  CO_RETURN_ON_ERROR(inode);
  if (inode->id != entry.id) {
    XLOGF(DFATAL, "{}.id != {}.id", inode, entry);
    co_return makeError(MetaCode::kFoundBug);
  }
  if (inode->isDirectory() && inode->asDirectory().parent != entry.parent) {
    XLOGF(DFATAL, "entry {}, inode {}, different parent", entry, *inode);
    co_return makeError(MetaCode::kFoundBug);
  }

  bool perm = true;
  if (inode->acl.iflags & FS_IMMUTABLE_FL) {
    perm = false;
  } else if (manager.config_.gc().recursive_perm_check() && inode->isDirectory()) {
    auto check = inode->acl.checkRecursiveRmPerm(flat::UserInfo(user.uid, user.gid, user.groups), false);
    if (check.hasError()) {
      // allow remove empty directory
      auto empty = co_await DirEntryList::checkEmpty(txn, inode->id);
      CO_RETURN_ON_ERROR(empty);
      perm = *empty;
    }
  }

  if (!perm) {
    // no permission to remove, move into orphan directory
    auto orphanEntry = co_await createOrphanEntry(manager, txn, entry, *inode, user);
    CO_RETURN_ON_ERROR(orphanEntry);
    XLOGF(CRITICAL, "no permission to perform recursive remove {}, move to {}", entry, *orphanEntry);
    if (inode->isDirectory()) {
      inode->asDirectory().parent = orphanEntry->parent;
      CO_RETURN_ON_ERROR(co_await inode->store(txn));
    }
    CO_RETURN_ON_ERROR(co_await entry.remove(txn));
    co_return Void{};
  }

  // can remove this entry
  auto gcInfo = GcInfo();
  gcInfo.user = taskEntry.gcInfo ? taskEntry.gcInfo->user : taskEntry.dirAcl->uid;
  gcInfo.origPath = taskEntry.gcInfo ? taskEntry.gcInfo->origPath / entry.name : Path(entry.name);
  CO_RETURN_ON_ERROR(co_await manager.removeEntry(txn, entry, *inode, gcInfo));

  co_return Void{};
}

CoTryTask<DirEntry> GcManager::GcTask::createOrphanEntry(GcManager &manager,
                                                         IReadWriteTransaction &txn,
                                                         const DirEntry &entry,
                                                         const Inode &inode,
                                                         const flat::UserAttr &user) {
  XLOGF_IF(FATAL, entry.id != inode.id, "entry {}, inode {}", entry, inode);
  auto orphanDir = Path(fmt::format("trash/gc-orphans/{}-{:%Y%m%d}", user.name, UtcClock::now()));
  if (inode.isFile()) {
    orphanDir = orphanDir / fmt::format("{}", entry.parent);
  }
  auto orphanName = entry.name;

  auto allocateInodeId = [&]() -> CoTryTask<InodeId> {
    auto id = co_await manager.idAlloc_->allocate();
    CO_RETURN_ON_ERROR(id);
    auto load = co_await Inode::load(txn, *id);
    CO_RETURN_ON_ERROR(load);
    if (load->has_value()) {
      auto &inode = **load;
      XLOGF(FATAL, "Found duplicated InodeId {}, {}", *id, inode);
    }
    co_return *id;
  };

  // create orphan directory
  auto parent = InodeId::root();
  for (const auto &iter : orphanDir) {
    assert(!iter.empty());
    const auto &fname = iter.string();
    for (size_t i = 0; true; i++) {
      auto name = i == 0 ? fname : fmt::format("{}.{}", fname.substr(0, 240), i);
      auto entry = co_await DirEntry::load(txn, parent, name);
      CO_RETURN_ON_ERROR(entry);
      auto found = entry->has_value();
      if (found) {
        if (entry.value()->isDirectory()) {
          parent = entry.value()->id;
          break;
        }
        XLOGF(WARN, "entry {} exists, but not directory", **entry);
        continue;
      }
      auto id = co_await allocateInodeId();
      CO_RETURN_ON_ERROR(id);
      auto acl = Acl(flat::Uid(0), flat::Gid(0), flat::Permission(0755));
      auto newEntry = DirEntry::newDirectory(parent, name, *id, acl);
      CO_RETURN_ON_ERROR(co_await newEntry.store(txn));
      auto newInode = Inode::newDirectory(*id, parent, name, acl, Layout(), UtcClock::now().castGranularity(1_s));
      CO_RETURN_ON_ERROR(co_await newInode.store(txn));
      parent = *id;
      break;
    }
  }

  if (taskEntry.gcInfo) {
    // create a symlink under directory, give original path
    auto symlinkParent = inode.isDirectory() ? inode.id : parent;
    auto origPath = inode.isDirectory() ? taskEntry.gcInfo->origPath / entry.name : taskEntry.gcInfo->origPath;
    for (size_t i = 0; true; i++) {
      auto symlinkName =
          i == 0 ? "_hf3fs_original_path" : fmt::format("_hf3fs_original_path.{}", UtcClock::now().toMicroseconds());
      auto entry = co_await DirEntry::load(txn, symlinkParent, symlinkName);
      CO_RETURN_ON_ERROR(entry);
      if (entry->has_value()) {
        if (entry->value().isSymlink()) {
          auto inode = co_await entry->value().loadInode(txn);
          CO_RETURN_ON_ERROR(inode);
          if (inode->asSymlink().target == origPath) {
            break;
          }
        }
        continue;
      }
      auto id = co_await allocateInodeId();
      CO_RETURN_ON_ERROR(id);
      auto symlinkInode = Inode::newSymlink(*id,
                                            origPath,
                                            taskEntry.gcInfo->user,
                                            flat::Gid(taskEntry.gcInfo->user.toUnderType()),
                                            UtcClock::now().castGranularity(1_s));
      auto symlinkEntry = DirEntry::newSymlink(symlinkParent, symlinkName, *id);
      CO_RETURN_ON_ERROR(co_await symlinkInode.store(txn));
      CO_RETURN_ON_ERROR(co_await symlinkEntry.store(txn));
      break;
    }
  }

  for (size_t i = 0; true; i++) {
    auto name = i == 0 ? orphanName : fmt::format("{}.{}", orphanName.substr(0, 230), UtcClock::now().toMicroseconds());
    auto check = co_await DirEntry::checkExist(txn, parent, name);
    CO_RETURN_ON_ERROR(check);
    if (auto exists = *check; !exists) {
      auto orphanEntry = entry;
      orphanEntry.parent = parent;
      orphanEntry.name = name;
      CO_RETURN_ON_ERROR(co_await orphanEntry.store(txn));
      co_return orphanEntry;
    }
  }
}

CoTryTask<void> GcManager::GcTask::removeGcEntryAndInode(GcManager &manager, IReadWriteTransaction &txn) {
  CO_RETURN_ON_ERROR(co_await taskEntry.remove(txn, true));
  CO_RETURN_ON_ERROR(co_await Inode(taskEntry.id).remove(txn));
  co_return Void{};
}

CoTryTask<void> GcManager::init() {
  XLOGF(INFO, "GcManager::init");

  auto check = co_await checkFs();
  if (check.hasError()) {
    XLOGF(ERR, "GcManager::checkFs failed, error {}", check.error());
    CO_RETURN_ERROR(check);
  }

  // each meta server may have 5 GC directory
  for (size_t i = 0; i < kNumGcDirectoryPerServer; i++) {
    auto gcDirectory = co_await openGcDirectory(i, i != 0);
    if (gcDirectory.hasError()) {
      XLOGF(ERR, "GcManager::openGcDirectory({}, {}) failed, error {}", i, i != 0, gcDirectory.error());
      CO_RETURN_ERROR(gcDirectory);
    }
    if (*gcDirectory) {
      currGcDirectories_.push_back(*gcDirectory);
    }
  }
  XLOGF_IF(FATAL, currGcDirectories_.empty(), "currGcDirectories_.empty()");

  XLOGF(INFO, "GcManager::init success.");
  co_return Void{};
}

void GcManager::start(CPUExecutorGroup &exec) {
  XLOGF(DBG, "GcManager start.");
  XLOGF_IF(FATAL, currGcDirectories_.empty(), "GcDirectory not set!!!");

  // start GC workers
  gcWorkers_ = std::make_unique<PriorityCoroutinePool<GcTask>>(config_.gc().workers());
  gcWorkers_->start(folly::partial(&GcManager::runGcTask, this), exec);

  // start GC scanner
  gcRunner_ = std::make_unique<BackgroundRunner>(&exec.pickNext());
  gcRunner_->start(
      "ScanAllGcDirs",
      [&]() -> CoTask<void> {
        if (config_.gc().enable()) {
          auto result = co_await this->scanAllGcDirectories();
          XLOGF_IF(ERR,
                   result.hasError(),
                   "GcManager failed to scan all available GC directories, error {}",
                   result.error());
        }
      },
      []() { return 30_s; });

  for (const auto &gcDir : currGcDirectories_) {
    gcDir->start(*this, exec);
  }

  XLOGF(INFO, "GcManager started!");
}

void GcManager::stopAndJoin() {
  XLOGF(INFO, "GcManager stop.");
  for (const auto &gcDir : currGcDirectories_) {
    gcDir->stopAndJoin();
  }
  if (gcRunner_) {
    folly::coro::blockingWait(gcRunner_->stopAll());
    gcRunner_.reset();
  }
  if (gcWorkers_) {
    gcWorkers_->stopAndJoin();
    gcWorkers_.reset();
  }
  XLOGF(INFO, "GcManager stopped!");
}

CoTryTask<void> GcManager::checkFs() {
  co_return co_await runReadOnly([&](auto &txn) -> CoTryTask<void> {
    // check tree roots exist
    auto exists = [](auto &val) { return val.has_value(); };
    auto root = (co_await Inode::snapshotLoad(txn, InodeId::root())).then(exists);
    auto gcRoot = (co_await Inode::snapshotLoad(txn, InodeId::gcRoot())).then(exists);
    CO_RETURN_ON_ERROR(root);
    CO_RETURN_ON_ERROR(gcRoot);
    if (!*root || !*gcRoot) {
      XLOGF(CRITICAL, "Root or GcRoot not found, root {}, gcRoot {}", root, gcRoot);
      co_return makeError(MetaCode::kBadFileSystem);
    }
    co_return Void{};
  });
}

CoTryTask<std::shared_ptr<GcManager::GcDirectory>> GcManager::openGcDirectory(size_t idx, bool create) {
  // generate GC directory name based on nodeId
  auto gcDirectoryName = GcDirectory::nameOf(nodeId_, idx);
  XLOGF(INFO, "Open GC directory {}/{}", InodeId::gcRoot(), gcDirectoryName);

  co_return co_await runReadWrite([&](IReadWriteTransaction &txn) -> CoTryTask<std::shared_ptr<GcDirectory>> {
    auto entry = co_await DirEntry::load(txn, InodeId::gcRoot(), gcDirectoryName);
    CO_RETURN_ON_ERROR(entry);
    if (entry->has_value()) {
      XLOGF(INFO, "GC directory {}/{} -> {} exists", InodeId::gcRoot(), gcDirectoryName, **entry);
      co_return std::make_shared<GcDirectory>(**entry);
    }
    if (!create) {
      co_return std::shared_ptr<GcDirectory>();
    }

    // chose InodeId randomly
    std::vector<InodeId> inodeIds;
    while (inodeIds.size() < 512) {
      auto newId = co_await idAlloc_->allocate();
      CO_RETURN_ON_ERROR(newId);
      inodeIds.push_back(*newId);
    }
    std::shuffle(inodeIds.begin(), inodeIds.end(), std::mt19937(std::random_device()()));

    auto newId = inodeIds.front();
    auto gcDir = Inode::newDirectory(newId,
                                     InodeId::gcRoot(),
                                     gcDirectoryName,
                                     Acl::gcRoot(),
                                     Layout() /* Invalid layout */,
                                     UtcClock::now());
    auto gcEntry = DirEntry::newDirectory(InodeId::gcRoot(), gcDirectoryName, newId, Acl::gcRoot());
    XLOGF(INFO,
          "GC directory {}/{} not found, create it: id {}, entry {}.",
          InodeId::gcRoot(),
          gcDirectoryName,
          newId,
          gcEntry);
    CO_RETURN_ON_ERROR(co_await gcDir.store(txn));
    CO_RETURN_ON_ERROR(co_await gcEntry.store(txn));

    co_return std::make_shared<GcDirectory>(gcEntry);
  });
}

CoTryTask<void> GcManager::removeEntry(IReadWriteTransaction &txn, const DirEntry &entry, Inode &inode, GcInfo gcInfo) {
  XLOGF(DBG, "GcManager remove entry {}", entry);
  if (inode.nlink == 0) {
    auto msg = fmt::format("DirEntry {} exists, but inode {}'s nlink is 0, shouldn't happen!!!", entry, inode);
    XLOG(DFATAL, msg);
    co_return makeError(MetaCode::kInconsistent, msg);
  }
  if (inode.acl.iflags & FS_IMMUTABLE_FL) {
    auto msg = fmt::format("can't remove inode {} with FS_IMMUTABLE_FL", inode.id);
    XLOG(CRITICAL, msg);
    co_return makeError(MetaCode::kNoPermission, msg);
  }

  inode.nlink--;
  SetAttr::update(inode.ctime, UtcClock::now(), config_.time_granularity(), true /* cmp */);

  // add into read conflict set
  CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(txn));
  CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));

  if (entry.isSymlink()) {
    // remove symlink
    CO_RETURN_ON_ERROR(co_await entry.remove(txn));
    if (inode.nlink != 0) {
      CO_RETURN_ON_ERROR(co_await inode.store(txn));
    } else {
      CO_RETURN_ON_ERROR(co_await inode.remove(txn));
    }
  } else {
    // remove directory or file
    CO_RETURN_ON_ERROR(co_await inode.store(txn));
    CO_RETURN_ON_ERROR(co_await entry.remove(txn));

    if (inode.isDirectory()) {
      if (inode.nlink != 0) {
        XLOGF(DFATAL, "Directory {} nlink != 0", inode);
        co_return makeError(MetaCode::kFoundBug);
      }
      if (inode.asDirectory().parent != entry.parent) {
        XLOGF(DFATAL, "Directory inode {}, entry {}, parent not match", inode.asDirectory(), entry);
        co_return makeError(MetaCode::kFoundBug);
      }
    }

    if (inode.nlink != 0) {
      // this is not last reference, can't remove
      XLOGF(DBG, "Inode {} has {} links after remove {}", inode.id, inode.nlink, entry);
    } else {
      auto gcDirectory = pickGcDirectory();
      CO_RETURN_ON_ERROR(co_await gcDirectory->add(txn, inode, config_.gc(), gcInfo));
    }
  }
  co_return Void{};
}

CoTryTask<void> GcManager::scanAllGcDirectories() {
  std::vector<DirEntry> entries;
  while (true) {
    auto result = co_await runReadOnly([&](auto &txn) -> CoTryTask<DirEntryList> {
      auto prev = entries.empty() ? "" : entries.back().name;
      co_return co_await DirEntryList::snapshotLoad(txn, InodeId::gcRoot(), prev, 128);
    });
    CO_RETURN_ON_ERROR(result);
    entries.insert(entries.end(), result->entries.begin(), result->entries.end());
    if (!result->more) {
      break;
    }
  }

  std::map<std::string, flat::NodeId> active;
  if (auto routing = mgmtd_->getRoutingInfo(); routing) {
    auto nodes = routing->getNodeBy(flat::selectNodeByType(flat::NodeType::META) && flat::selectActiveNode());
    for (auto &node : nodes) {
      // skip GC directory 0
      for (size_t i = 1; i < kNumGcDirectoryPerServer; i++) {
        active.insert_or_assign(GcDirectory::nameOf(node.app.nodeId, i), node.app.nodeId);
      }
    }
  }

  std::vector<std::shared_ptr<GcDirectory>> activeDirs;
  std::set<std::string> activeNames, inactiveNames;
  for (auto &entry : entries) {
    if (active.contains(entry.name) || std::any_of(currGcDirectories_.begin(),
                                                   currGcDirectories_.end(),
                                                   [&](const auto &gcDir) { return entry.name == gcDir->name(); })) {
      activeDirs.emplace_back(std::make_shared<GcDirectory>(entry));
      activeNames.insert(entry.name);
    } else {
      inactiveNames.insert(entry.name);
    }
  }

  XLOGF(INFO,
        "GcManager found {} GC directories, active {}, inactive {}",
        entries.size(),
        fmt::join(activeNames.begin(), activeNames.end(), ","),
        fmt::join(inactiveNames.begin(), inactiveNames.end(), ","));
  for (const auto &gcDir : currGcDirectories_) {
    XLOGF_IF(FATAL,
             !activeNames.contains(gcDir->name()),
             "Current GC Directory {} not found under GcRoot",
             gcDir->name());
  }
  allGcDirectories_.withWLock([&](auto &val) { val = activeDirs; });

  co_return Void{};
}

bool GcManager::enableGcDelay() const {
  if (auto fsStatus = fileHelper_->cachedFsStatus(); fsStatus.has_value()) {
    auto free = (double)fsStatus->free / fsStatus->capacity * 100;
    if (free < config_.gc().gc_delay_free_space_threshold()) {
      XLOGF_EVERY_MS(WARN,
                     5000,
                     "free space {} < {}, disable GC delay",
                     free,
                     config_.gc().gc_delay_free_space_threshold());
      return false;
    }
    return true;
  }
  XLOGF_EVERY_MS(WARN, 5000, "GcManager failed to get FsStatus");

  return true;
}

CoTask<void> GcManager::runGcTask(GcTask task) {
  SCOPE_EXIT { task.gcDir->finish(task); };

  if (!config_.gc().enable()) {
    co_return;
  }

  auto begin = SteadyClock::now();
  auto result = co_await task.run(*this);

  if (result.hasError()) {
    XLOGF(ERR, "GC {} failed, error {}", task.taskEntry.id, result.error());
    gcFailCount.addSample(1);
  } else {
    XLOGF(DBG, "GC {} success", task.taskEntry);
    gcSuccCount.addSample(1, {{"instance", task.taskEntry.isDirectory() ? "directory" : "file"}});
    gcLatency.addSample(SteadyClock::now() - begin);
  }

  co_return;
}

}  // namespace hf3fs::meta::server
