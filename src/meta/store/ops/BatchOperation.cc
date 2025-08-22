#include "BatchOperation.h"

#include <algorithm>
#include <bits/ranges_algo.h>
#include <boost/iterator/transform_iterator.hpp>
#include <cassert>
#include <exception>
#include <fcntl.h>
#include <folly/Likely.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/functional/Partial.h>
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/event/Event.h"
#include "meta/store/Inode.h"
#include "meta/store/Operation.h"
#include "meta/store/ops/SetAttr.h"

#define CO_RETURN_ON_TXN_ERROR(result)                                                           \
  do {                                                                                           \
    auto &&_r = result;                                                                          \
    if (_r.hasError() && StatusCode::typeOf(_r.error().code()) == StatusCodeType::Transaction) { \
      CO_RETURN_ERROR(_r);                                                                       \
    }                                                                                            \
  } while (0)

namespace hf3fs::meta::server {
namespace {
monitor::CountRecorder batchCnt("meta_server.batch_op_size");
}
extern monitor::CountRecorder openWrite;

/** BatchedOp */
CoTryTask<Inode> BatchedOp::run(IReadWriteTransaction &txn) {
  auto dist = co_await distributor().checkOnServer(txn, inodeId_);
  CO_RETURN_ON_ERROR(dist);
  auto [ok, versionstamp] = *dist;
  if (!ok) {
    XLOGF(INFO, "inode {} not on current server, need retry", inodeId_);
    co_return makeError(MetaCode::kBusy, "inode not on server, retry");
  }

  auto inode = (co_await Inode::snapshotLoad(txn, inodeId_)).then(checkMetaFound<Inode>);
  CO_RETURN_ON_ERROR(inode);

  // sanity check for file length, if we hold the lock, and versionstamp not changed file length shouldn't changed
  if (inode->isFile()) {
    if (versionstamp != versionstamp_) {
      currLength_ = inode->asFile().getVersionedLength();
      nextLength_ = std::nullopt;
      versionstamp_ = versionstamp;
    }

    if (currLength_ != inode->asFile().getVersionedLength() && nextLength_ != inode->asFile().getVersionedLength()) {
      // we should never see this if all meta server is up to date
      XLOGF(DFATAL,
            "file {} length updated during operation, {} != {}",
            *currLength_,
            inode->asFile().getVersionedLength());
      co_return makeError(MetaCode::kBusy, "length updated during operation, retry");
    }
  }

  // handle all sync and close operation
  auto r1 = co_await syncAndClose(txn, *inode);
  CO_RETURN_ON_ERROR(r1);

  auto r2 = co_await setAttr(txn, *inode);
  CO_RETURN_ON_ERROR(r2);

  auto r3 = co_await create(txn, *inode);
  CO_RETURN_ON_ERROR(r3);

  auto dirty = *r1 || *r2 || *r3;
  if (dirty) {
    // NOTE: add inode into read conflict set
    CO_RETURN_ON_ERROR(co_await inode->addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await inode->store(txn));
  }

  co_return *inode;
}

CoTryTask<bool> BatchedOp::syncAndClose(IReadWriteTransaction &txn, Inode &inode) {
  std::vector<FileSession> sessions;
  std::optional<VersionedLength> hintLength;
  bool updateLength = false;
  bool truncate = false;
  bool dirty = false;

  // initial hint length
  hintLength = meta::VersionedLength{0, 0};

  // merge all requests
  for (auto &waiter : syncs_) {
    auto result = co_await sync(inode, waiter.get().req, updateLength, truncate, hintLength);
    if (result.hasError()) {
      waiter.get().result = makeError(std::move(result.error()));
    } else {
      dirty |= *result;
    }
  }
  for (auto &waiter : closes_) {
    auto result = co_await close(inode, waiter.get().req, updateLength, hintLength, sessions);
    if (result.hasError()) {
      waiter.get().result = makeError(std::move(result.error()));
    } else {
      dirty |= *result;
    }
  }

  if (truncate) {
    // ignore hint length when truncate happened
    hintLength = std::nullopt;
    updateLength = true;
  }

  // remove sessions
  for (auto &session : sessions) {
    CO_RETURN_ON_ERROR(co_await session.remove(txn));
  }

  if (!updateLength) {
    // we don't need updateLength, just return
    co_return dirty;
  }

  if (!inode.isFile()) {
    XLOGF(DFATAL, "{} updateLength but not file, shouldn't happen", inode);
    co_return makeError(MetaCode::kFoundBug, "updateLength but not file");
  }

  auto newLength = co_await queryLength(inode, hintLength, truncate);
  CO_RETURN_ON_ERROR(newLength);
  nextLength_ = *newLength;
  if (*newLength != inode.asFile().getVersionedLength()) {
    XLOGF_IF(FATAL,
             (newLength->truncateVer < inode.asFile().truncateVer ||
              (newLength->truncateVer == inode.asFile().truncateVer && newLength->length < inode.asFile().length)),
             "file {}, newLength {} currLength {}",
             inode.id,
             *newLength,
             inode.asFile().getVersionedLength());
    XLOGF(DBG, "{} changed, {} != {}", inode.id, *newLength, inode.asFile().getVersionedLength());

    SetAttr::update(inode.mtime, UtcClock::now(), config().time_granularity(), true);
    if (newLength->truncateVer != inode.asFile().truncateVer) {
      SetAttr::update(inode.ctime, UtcClock::now(), config().time_granularity(), true);
    }

    inode.asFile().setVersionedLength(*newLength);
    dirty = true;
  } else {
    XLOGF(DBG,
          "{} length not changed, length {} == {}, {} {}",
          inode.id,
          *newLength,
          inode.asFile().getVersionedLength(),
          *newLength != inode.asFile().getVersionedLength(),
          *newLength == inode.asFile().getVersionedLength());
  }

  co_return dirty;
}

CoTryTask<bool> BatchedOp::sync(Inode &inode,
                                const SyncReq &req,
                                bool &updateLength,
                                bool &truncate,
                                std::optional<VersionedLength> &hintLength) {
  // check request
  CO_RETURN_ON_ERROR(req.valid());
  if (req.inode != PathAt(inodeId_)) {
    XLOGF(DFATAL, "SyncReq {} shouldn't in batch of {}", req, inodeId_);
    co_return makeError(MetaCode::kFoundBug, "Invalid batchOp");
  }
  if ((req.updateLength || req.truncated || req.lengthHint) && !inode.isFile()) {
    co_return makeError(MetaCode::kNotFile, "update length but not file");
  }
  if (req.lengthHint && req.lengthHint->truncateVer > inode.asFile().truncateVer) {
    auto msg = fmt::format("inode {} hint truncateVer {} > current truncateVer {}",
                           inodeId_,
                           req.lengthHint->truncateVer,
                           inode.asFile().truncateVer);
    XLOG(DFATAL, msg);
    co_return makeError(MetaCode::kFoundBug, std::move(msg));
  }

  bool dirty = false;
  dirty |= SetAttr::update(inode.atime, req.atime, config().time_granularity(), true /* cmp */);
  dirty |= SetAttr::update(inode.mtime, req.mtime, config().time_granularity(), true /* cmp */);
  if (req.truncated) {
    dirty |=
        SetAttr::update(inode.ctime, req.mtime.value_or(UtcClock::now()), config().time_granularity(), true /* cmp */);
  }

  updateLength |= req.updateLength;
  if (req.updateLength) {
    hintLength = VersionedLength::mergeHint(hintLength, req.lengthHint);
  }
  truncate |= req.truncated;

  co_return dirty;
}

CoTryTask<bool> BatchedOp::close(Inode &inode,
                                 const CloseReq &req,
                                 bool &updateLength,
                                 std::optional<VersionedLength> &hintLength,
                                 std::vector<FileSession> &sessions) {
  CO_RETURN_ON_ERROR(req.valid());
  if (req.inode != PathAt(inodeId_)) {
    XLOGF(DFATAL, "CloseReq {} shouldn't batch of {}", req, inodeId_);
    co_return makeError(MetaCode::kFoundBug, "Invalid batchOp");
  }

  if ((req.session || req.updateLength) && !inode.isFile()) {
    co_return makeError(MetaCode::kNotFile);
  }

  bool dirty = false;
  dirty |= SetAttr::update(inode.atime, req.atime, config().time_granularity(), true /* cmp */);
  dirty |= SetAttr::update(inode.mtime, req.mtime, config().time_granularity(), true /* cmp */);

  updateLength |= req.updateLength;
  if (req.updateLength) {
    hintLength = VersionedLength::mergeHint(hintLength, req.lengthHint);
  }

  if (req.session.has_value()) {
    sessions.push_back(FileSession::create(inode.id, *req.session));
  }

  co_return dirty;
}

CoTryTask<VersionedLength> BatchedOp::queryLength(const Inode &inode,
                                                  std::optional<VersionedLength> hintLength,
                                                  bool truncate) {
  XLOGF_IF(FATAL, !inode.isFile(), "not file");
  XLOGF_IF(FATAL, truncate && hintLength, "truncate but hintLength {}", *hintLength);
  if (nextLength_) {
    XLOGF(DBG, "inode {} update to cached nextLength {}", inode, *nextLength_);
    co_return *nextLength_;
  }

  auto currLength = inode.asFile().getVersionedLength();
  if (hintLength && !config().ignore_length_hint()) {
    if (currLength.truncateVer >= hintLength->truncateVer && currLength.length >= hintLength->length) {
      XLOGF(DBG, "don't need update {}, current {}, hint {}", inode.id, currLength, *hintLength);
      co_return currLength;
    }
    if (hintLength->truncateVer == currLength.truncateVer && hintLength->length > currLength.truncateVer) {
      XLOGF(DBG, "update {} to hint {}, current {}", inode.id, *hintLength, currLength);
      co_return *hintLength;
    }
    XLOGF_IF(DFATAL,
             hintLength->truncateVer > currLength.truncateVer,
             "file {}, hint {} > {}!!!",
             inode.id,
             hintLength->truncateVer,
             currLength.truncateVer);
  }

  XLOGF(DBG,
        "need query length for {}, current {}, hint {}, ignore hint {}, truncate {}, sync {}, close {}",
        inode.id,
        currLength,
        OptionalFmt(hintLength),
        config().ignore_length_hint(),
        truncate,
        syncs_.size(),
        closes_.size());
  auto length = co_await fileHelper().queryLength(flat::UserInfo(user_), inode);
  CO_RETURN_ON_ERROR(length);
  XLOGF(DBG, "qeury length for {}, get {}", inode.id, *length);
  auto truncateVer =
      (truncate || *length < inode.asFile().length) ? inode.asFile().truncateVer + 1 : inode.asFile().truncateVer;
  co_return VersionedLength{*length, truncateVer};
}

CoTryTask<bool> BatchedOp::setAttr(IReadWriteTransaction &txn, Inode &inode) {
  auto dirty = false;
  auto oldAcl = inode.acl;
  for (auto &waiter : setattrs_) {
    const auto &req = waiter.get().req;
    if (req.path != PathAt(inodeId_)) {
      XLOGF(DFATAL, "SetAttrReq {} shouldn't in batch of {}", req, inodeId_);
      co_return makeError(MetaCode::kFoundBug, "Invalid batchOp");
    }
    auto result = SetAttr::check(inode, req, config());
    if (result.hasError()) {
      waiter.get().result = makeError(result.error());
    } else {
      dirty |= SetAttr::apply(inode, req, config().time_granularity(), config().dynamic_stripe_growth());
    }
  }

  if (inode.isDirectory() && inode.acl != oldAcl && inode.id != InodeId::root()) {
    XLOGF_IF(FATAL, !dirty, "acl changed but dirty not set");
    auto result = co_await inode.snapshotLoadDirEntry(txn);
    CO_RETURN_ON_ERROR(result);
    auto entry = DirEntry(*result);
    XLOGF_IF(DFATAL,
             (!inode.asDirectory().name.empty() && entry.name != inode.asDirectory().name),
             "{} != {}",
             entry.name,
             inode.asDirectory().name);
    if (inode.asDirectory().name.empty()) {
      inode.asDirectory().name = entry.name;
    }
    entry.dirAcl = inode.acl;
    CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await entry.store(txn));
  }

  co_return dirty;
}

CoTryTask<bool> BatchedOp::create(IReadWriteTransaction &txn, Inode &inode) {
  if (creates_.empty()) {
    co_return false;
  }

  if (!inode.isDirectory()) {
    for (auto &waiter : creates_) {
      waiter.get().result = makeError(MetaCode::kNotDirectory);
    }
    co_return false;
  }

  folly::Synchronized<uint32_t> chainAllocCounter(inode.asDirectory().chainAllocCounter);
  if (creates_.size() == 1) {
    auto result = co_await create(txn, inode, chainAllocCounter, creates_.begin(), creates_.end());
    CO_RETURN_ON_ERROR(result);
    co_return SetAttr::update(inode.asDirectory().chainAllocCounter, *chainAllocCounter.rlock()) || *result;
  }

  std::multimap<std::string, WaiterRef<CreateReq, CreateRsp>> map;
  for (auto &waiter : creates_) {
    const auto &name = waiter.get().req.path.path;
    if (UNLIKELY(!name || name->has_parent_path())) {
      auto msg = fmt::format("inode {}, create req {}", inodeId_, waiter.get().req);
      XLOG(DFATAL, msg);
      co_return makeError(MetaCode::kFoundBug, std::move(msg));
    }
    map.insert({name->string(), waiter});
  }

  std::vector<folly::SemiFuture<Result<bool>>> tasks;
  auto exec = co_await folly::coro::co_current_executor;
  auto dirty = false;

  auto convert = [](auto &iter) { return iter.second; };
  for (auto begin = map.begin(), end = std::next(begin); begin != map.end(); begin = end) {
    while (end != map.end() && end->first == begin->first) end++;

    auto ibegin = boost::make_transform_iterator(begin, convert);
    auto iend = boost::make_transform_iterator(end, convert);
    tasks.push_back(create(txn, inode, chainAllocCounter, ibegin, iend).scheduleOn(exec).start());

    if (tasks.size() >= 8 || end == map.end()) {
      auto results = co_await folly::coro::collectAllRange(std::exchange(tasks, {}));
      for (auto &res : results) {
        CO_RETURN_ON_ERROR(res);
        dirty |= *res;
      }
    }
  }
  assert(tasks.empty());
  dirty |= SetAttr::update(inode.asDirectory().chainAllocCounter, *chainAllocCounter.rlock());

  co_return dirty;
}

CoTryTask<bool> BatchedOp::create(IReadWriteTransaction &txn,
                                  const Inode &parent,
                                  folly::Synchronized<uint32_t> &chainAllocCounter,
                                  auto begin,
                                  auto end) {
  if (begin == end) {
    co_return false;
  }

  const auto &path = begin->get().req.path;
  XLOGF_IF(FATAL,
           (parent.id != inodeId_ || path.parent != inodeId_ || !path.path || path.path->has_parent_path()),
           "{}, {}, {}",
           parent.id,
           inodeId_,
           path);

  const auto &name = path.path->string();
  auto entry = co_await DirEntry::snapshotLoad(txn, inodeId_, name);
  CO_RETURN_ON_ERROR(entry);
  XLOGF(DBG, "entry {}/{} -> {}", inodeId_, name, OptionalFmt(*entry));

  if (entry->has_value()) {
    auto inode = co_await entry->value().snapshotLoadInode(txn);
    CO_RETURN_ON_ERROR(inode);
    co_return (co_await openExists(txn, *inode, **entry, begin, end)).then([](auto &) { return false; });
  }

  for (auto iter = begin; iter != end; iter++) {
    auto &waiter = iter->get();
    assert(!entry->has_value());

    auto result = co_await create(txn, parent, chainAllocCounter, waiter.req);
    CO_RETURN_ON_TXN_ERROR(result);
    if (result.hasError()) {
      waiter.result = makeError(std::move(result.error()));
    } else {
      auto &[inode, entry] = *result;
      waiter.result = CreateRsp(inode, false /* needTrunc */);
      waiter.newFile = true;
      co_return (co_await openExists(txn, inode, entry, std::next(iter), end)).then([](auto &) { return false; });
    }
  }

  co_return false;
}

CoTryTask<std::pair<Inode, DirEntry>> BatchedOp::create(IReadWriteTransaction &txn,
                                                        const Inode &parent,
                                                        folly::Synchronized<uint32_t> &chainAllocCounter,
                                                        const CreateReq &req) {
  CO_RETURN_ON_ERROR(req.valid());
  auto parentId = inodeId_;
  auto parentAcl = parent.acl;
  const auto &name = req.path.path->string();

  if (!parent.nlink) {
    co_return makeError(MetaCode::kNotFound, fmt::format("{}, Directory {} is removed", req.path, parentId));
  }

  CO_RETURN_ON_ERROR(req.path.validForCreate());
  CO_RETURN_ON_ERROR(parentAcl.checkPermission(req.user, AccessType::WRITE));
  CO_RETURN_ON_ERROR(parent.asDirectory().checkLock(req.client));

  auto layout = req.layout;
  if (!layout.has_value()) {
    // user doesn't specific layout, inherit parent directory's layout.
    layout = parent.asDirectory().layout;
  }

  if (!layout->empty()) {
    CO_RETURN_ON_ERROR(co_await chainAlloc().checkLayoutValid(*layout));
  } else {
    if (parent.acl.iflags & FS_CHAIN_ALLOCATION_FL) {
      CO_RETURN_ON_ERROR(co_await chainAlloc().allocateChainsForLayout(*layout, chainAllocCounter));
    } else {
      CO_RETURN_ON_ERROR(co_await chainAlloc().allocateChainsForLayout(*layout));
    }
  }

  auto newChunkEngine = config().enable_new_chunk_engine() || (parent.acl.iflags & FS_NEW_CHUNK_ENGINE);
  auto inodeId = co_await allocateInodeId(txn, newChunkEngine);
  CO_RETURN_ON_ERROR(inodeId);
  XLOGF_IF(FATAL,
           inodeId->useNewChunkEngine() != newChunkEngine,
           "InodeId {}, use new chunk engine {}",
           inodeId,
           newChunkEngine);

  auto entry = DirEntry::newFile(parentId, name, *inodeId);
  entry.uuid = req.uuid;
  auto inode = Inode::newFile(*inodeId,
                              Acl(req.user.uid, req.user.gid, meta::Permission(req.perm & ALLPERMS)),
                              std::move(*layout),
                              now());
  if (config().dynamic_stripe() && req.dynStripe) {
    inode.asFile().dynStripe = std::min(config().dynamic_stripe_initial(), inode.asFile().layout.stripeSize);
  }

  if (parentAcl.perm & S_ISGID) {
    // The set-group-ID bit (S_ISGID) has several special uses.
    // For a directory, it indicates that BSD semantics are to be used for that directory:
    // files created there inherit their group ID from the directory, not from the effective group ID of the creating
    // process, and directories created there will also get the S_ISGID bit set
    inode.acl.gid = parentAcl.gid;
  }

  // NOTE: add parent inode and dirEntry into read conflict set.
  // add parent inode into read conflict set to prevent parent is removed concurrently
  CO_RETURN_ON_ERROR(co_await Inode(parentId).addIntoReadConflict(txn));
  // add directory entry into read conflict set to prevent concurrent create
  CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));

  // create inode and dirEntry
  CO_RETURN_ON_ERROR(co_await entry.store(txn));
  CO_RETURN_ON_ERROR(co_await inode.store(txn));

  if (req.session && req.flags.accessType() != AccessType::READ) {
    openWrite.addSample(1);
    CO_RETURN_ON_ERROR(co_await FileSession::create(inode.id, req.session.value()).store(txn));
  }

  co_return std::make_pair(inode, entry);
}

CoTryTask<void> BatchedOp::openExists(IReadWriteTransaction &txn,
                                      Inode &inode,
                                      const DirEntry &entry,
                                      auto begin,
                                      auto end) {
  bool dirty = false;
  for (auto iter = begin; iter != end; iter++) {
    auto &waiter = iter->get();
    auto &req = waiter.req;
    if (entry.uuid != Uuid::zero() && entry.uuid == req.uuid) {
      // this may happens when FDB returns commit_unknown_result, or we failed to send response to client
      XLOGF(CRITICAL, "Create already finished, dst {}, req {}, uuid {}", entry, req, req.uuid);
      waiter.result = CreateRsp(inode, false /* trunc */);
      continue;
    }
    auto result = co_await openExists(txn, inode, req);
    CO_RETURN_ON_TXN_ERROR(result);
    if (result.hasError()) {
      waiter.result = makeError(std::move(result.error()));
    } else {
      waiter.result = CreateRsp(inode, waiter.req.flags.contains(O_TRUNC) /* needTrunc */);
      dirty |= *result;
    }
  }

  if (dirty) {
    CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await inode.store(txn));
  }

  co_return Void{};
}

CoTryTask<bool> BatchedOp::openExists(IReadWriteTransaction &txn, Inode &inode, const CreateReq &req) {
  CO_RETURN_ON_ERROR(req.valid());
  if (inode.isSymlink()) {
    // todo: rarely happens, how to handle this gracefully?
    auto msg = fmt::format("req {}, found symlink {}", req, inode);
    XLOG(WARN, msg);
    co_return makeError(MetaCode::kBusy, std::move(msg));
  }
  if (!inode.isFile()) {
    assert(inode.isDirectory());
    co_return makeError(MetaCode::kIsDirectory);
  }
  if (req.flags.contains(O_EXCL)) {
    co_return makeError(MetaCode::kExists);
  }

  // check permission
  if (req.flags.accessType() != AccessType::READ && (inode.acl.iflags & FS_IMMUTABLE_FL)) {
    co_return makeError(MetaCode::kNoPermission, fmt::format("FS_IMMUTABLE_FL set on inode {}", inode.id));
  }
  CO_RETURN_ON_ERROR(inode.acl.checkPermission(req.user, req.flags.accessType()));
  // check hole
  auto rdonly = req.flags.accessType() == AccessType::READ;
  if (rdonly && inode.asFile().hasHole() && config().check_file_hole()) {
    XLOGF(WARN, "Inode {} contains hole, don't allow O_RDONLY", inode.id);
    co_return makeError(MetaCode::kFileHasHole);
  }

  auto dirty = false;
  // clear SUID SGID sticky bits on write by non owner
  constexpr uint32_t sbits = S_ISUID | S_ISGID | S_ISVTX;
  static_assert(sbits == 07000);
  if (!rdonly && req.user.uid != inode.acl.uid && (inode.acl.perm & sbits)) {
    dirty |= SetAttr::update(inode.acl.perm, Permission(inode.acl.perm & (~sbits)));
  }
  // update dynamic stripe
  if (req.session.has_value() && req.flags.accessType() != AccessType::READ) {
    CO_RETURN_ON_ERROR(inode.acl.checkPermission(req.user, req.flags.accessType()));
    if (!req.dynStripe && inode.asFile().dynStripe && inode.asFile().dynStripe < inode.asFile().layout.stripeSize) {
      dirty |= SetAttr::update(inode.asFile().dynStripe, 0u);
    }
  }
  // create session
  if (req.session && req.flags.accessType() != AccessType::READ) {
    openWrite.addSample(1);
    CO_RETURN_ON_ERROR(co_await FileSession::create(inode.id, req.session.value()).store(txn));
  }
  co_return dirty;
}

void BatchedOp::retry(const Status &error) {
  Operation<Inode>::retry(error);
  for (auto &waiter : syncs_) {
    waiter.get().result = std::nullopt;
  }
  for (auto &waiter : closes_) {
    waiter.get().result = std::nullopt;
  }
  for (auto &waiter : setattrs_) {
    waiter.get().result = std::nullopt;
  }
  for (auto &waiter : creates_) {
    waiter.get().result = std::nullopt;
  }
}

/** BatchedOp::Waiter */
template <>
void BatchedOp::Waiter<CreateReq, CreateRsp>::finish(BatchedOp &op, const Result<Inode> &r) {
  SCOPE_EXIT { baton.post(); };
  if (r.hasError() && !result.has_value()) {
    result = makeError(r.error());
    return;
  }
  XLOGF_IF(FATAL, !result.has_value(), "req {}, no result", req);
  if (result->hasError()) {
    return;
  }

  auto inode = result->value().stat;
  if (newFile) {
    op.addEvent(Event::Type::Create)
        .addField("parent", req.path.parent)
        .addField("name", req.path.path->string())
        .addField("inode", inode.id)
        .addField("user", req.user.uid)
        .addField("host", req.client.hostname)
        .addField("chain_table", inode.asFile().layout.tableId);
  }
  if (req.session && req.flags.accessType() != AccessType::READ) {
    if (req.flags.contains(O_TRUNC) && !newFile) {
      result.value()->needTruncate = true;
    }
    op.addEvent(Event::Type::OpenWrite)
        .addField("inode", inode.id)
        .addField("owner", inode.acl.uid)
        .addField("user", req.user.uid)
        .addField("host", req.client.hostname)
        .addField("length", inode.asFile().length)
        .addField("truncateVer", inode.asFile().truncateVer)
        .addField("dynStripe", inode.asFile().dynStripe)
        .addField("otrunc", req.flags.contains(O_TRUNC));
  }
}

template <>
void BatchedOp::Waiter<SyncReq, SyncRsp>::finish(BatchedOp &op, const Result<Inode> &r) {
  if (!result.has_value()) {
    result = r.then([](auto &inode) { return SyncRsp(inode); });
  }
  if (req.truncated && !hasError()) {
    auto &inode = result.value()->stat;
    XLOGF_IF(DFATAL, !inode.isFile(), "req {} success, but inode {} is not file", req, inode);
    if (inode.isFile()) {
      op.addEvent(Event::Type::Truncate)
          .addField("inode", inode.id)
          .addField("length", inode.asFile().length)
          .addField("truncateVer", inode.asFile().truncateVer)
          .addField("dynStripe", inode.asFile().dynStripe)
          .addField("user", req.user.uid)
          .addField("host", req.client.hostname);
      op.addTrace(MetaEventTrace{
          .eventType = Event::Type::Truncate,
          .inodeId = inode.id,
          .userId = req.user.uid,
          .client = req.client,
          .length = inode.asFile().length,
          .truncateVer = inode.asFile().truncateVer,
          .dynStripe = inode.asFile().dynStripe,
      });
    }
  }
  baton.post();
}

template <>
void BatchedOp::Waiter<CloseReq, CloseRsp>::finish(BatchedOp &op, const Result<Inode> &r) {
  if (!result.has_value()) {
    result = r.then([](auto &inode) { return CloseRsp(inode); });
  }
  if (req.session && !hasError()) {
    auto &inode = result.value()->stat;
    XLOGF_IF(DFATAL, !inode.isFile(), "req {} success, but inode {} is not file", req, inode);
    if (inode.isFile()) {
      op.addEvent(Event::Type::CloseWrite)
          .addField("inode", inode.id.toHexString())
          .addField("owner", inode.acl.uid)
          .addField("user", req.user.uid)
          .addField("host", req.client.hostname)
          .addField("length", inode.asFile().length)
          .addField("truncateVer", inode.asFile().truncateVer)
          .addField("dynStripe", inode.asFile().dynStripe)
          .addField("prune", req.pruneSession);
      op.addTrace(MetaEventTrace{
          .eventType = Event::Type::CloseWrite,
          .inodeId = inode.id,
          .ownerId = inode.acl.uid,
          .userId = req.user.uid,
          .client = req.client,
          .length = inode.asFile().length,
          .truncateVer = inode.asFile().truncateVer,
          .dynStripe = inode.asFile().dynStripe,
          .pruneSession = req.pruneSession,
      });
    }
  }
  baton.post();
}

template <>
void BatchedOp::Waiter<SetAttrReq, SetAttrRsp>::finish(BatchedOp &, const Result<Inode> &r) {
  if (!result.has_value()) {
    result = r.then([](auto &inode) { return SetAttrRsp(inode); });
  }
  baton.post();
}

void BatchedOp::finish(const Result<Inode> &result) {
  batchCnt.addSample(syncs_.size() + closes_.size() + setattrs_.size());
  for (auto &waiter : syncs_) {
    waiter.get().finish(*this, result);
  }
  for (auto &waiter : closes_) {
    waiter.get().finish(*this, result);
  }
  for (auto &waiter : setattrs_) {
    waiter.get().finish(*this, result);
  }
  for (auto &waiter : creates_) {
    waiter.get().finish(*this, result);
  }
  Operation<Inode>::finish(result);
}

}  // namespace hf3fs::meta::server