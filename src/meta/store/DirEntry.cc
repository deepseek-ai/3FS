#include "meta/store/DirEntry.h"

#include <algorithm>
#include <boost/core/ignore_unused.hpp>
#include <cassert>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/Likely.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/functional/Partial.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <linux/limits.h>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/monitor/Recorder.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "meta/store/BatchContext.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {
namespace {
constexpr auto prefix = kv::KeyPrefix::Dentry;

bool checkName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." && std::find(name.begin(), name.end(), '/') == name.end();
}
}  // namespace

/** DirEntry */
std::string DirEntry::packKey(InodeId parent, std::string_view name) {
  String buf;
  buf.reserve(sizeof(prefix) + sizeof(InodeId::Key) + name.size());
  Serializer ser{buf};
  ser.put(prefix);
  ser.put(parent.packKey());
  ser.putRaw(name.data(), name.size());

  return buf;
}

std::string DirEntry::packKey() const { return packKey(parent, name); }

Result<Void> DirEntry::unpackKey(const std::string_view key) {
  // todo: log more data
  Deserializer des(key);
  auto p = des.get<kv::KeyPrefix>();
  RETURN_ON_ERROR(p);
  assert(p.value() == prefix);
  auto parentKey = des.get<InodeId::Key>();
  RETURN_ON_ERROR(parentKey);
  parent = InodeId::unpackKey(parentKey.value());
  auto nameRes = des.getRawUntilEnd();
  RETURN_ON_ERROR(nameRes);
  name = *nameRes;

  return Void();
}

Result<DirEntry> DirEntry::newUnpacked(const std::string_view key, const std::string_view value) {
  DirEntry entry;
  if (auto result = entry.unpackKey(key); result.hasError()) {
    auto formattedKey =
        fmt::format("{:02x}", fmt::join((uint8_t *)key.data(), (uint8_t *)key.data() + key.length(), ","));
    XLOGF(CRITICAL,
          "Failed to deserialize dirEntry key {}, error {}, data corruption!!!",
          formattedKey,
          result.error());
    return makeError(StatusCode::kDataCorruption, fmt::format("deserialize dirEntry key {} failed", formattedKey));
  }
  if (auto des = serde::deserialize(entry.data(), value); des.hasError()) {
    XLOGF(CRITICAL,
          "Failed to deserialize dirEntry {}/{}, {}, data corruption!!!",
          entry.parent,
          entry.name,
          des.error());
    return makeError(StatusCode::kDataCorruption);
  }
  return std::move(entry);
}

template <const bool SNAPSHOT>
CoTryTask<std::optional<DirEntry>> DirEntry::loadImpl(IReadOnlyTransaction &txn,
                                                      InodeId parent,
                                                      std::string_view name) {
  auto func = SNAPSHOT ? &IReadOnlyTransaction::snapshotGet : &IReadOnlyTransaction::get;
  if (name.size() > NAME_MAX) {
    XLOGF(DBG, "length of name {} > {}", name, NAME_MAX);
    co_return makeError(MetaCode::kNameTooLong, fmt::format("{} > {}", name, NAME_MAX));
  }
  auto result = co_await (txn.*func)(packKey(parent, name));
  if (result.hasError()) {
    XLOGF(ERR, "Failed to load dirEntry {}/{}, error {}", parent, name, result.error());
    CO_RETURN_ERROR(result);
  }
  if (auto &value = *result; value.has_value()) {
    DirEntry entry(parent, std::string(name), {});
#ifndef NDEBUG
    entry.snapshotLoaded_ = SNAPSHOT;
#endif
    if (auto des = serde::deserialize(entry.data(), *value); des.hasError()) {
      XLOGF(CRITICAL, "Failed to deserialize dirEntry {}/{}, {}, data corruption!!", parent, name, des.error());
      co_return makeError(StatusCode::kDataCorruption);
    }
    co_return std::move(entry);
  } else {
    co_return std::nullopt;
  }
}

CoTryTask<std::optional<DirEntry>> DirEntry::snapshotLoad(IReadOnlyTransaction &txn,
                                                          InodeId parent,
                                                          std::string_view name) {
  if (auto batch = BatchContext::get(); batch) {
    auto guard = batch->loadDirEntry(parent, std::string(name));
    if (guard.needLoad) {
      auto r = co_await loadImpl<true>(txn, parent, name);
      guard.set(r);
      co_return std::move(r);
    } else {
      co_return co_await guard.coAwait();
    }
  } else {
    co_return co_await loadImpl<true>(txn, parent, name);
  }
}

CoTryTask<std::optional<DirEntry>> DirEntry::load(IReadOnlyTransaction &txn, InodeId parent, std::string_view name) {
  co_return co_await loadImpl<false>(txn, parent, name);
}

CoTryTask<void> DirEntry::store(IReadWriteTransaction &txn) const {
  assert(!snapshotLoaded_);
  if (UNLIKELY(!checkName(name))) {
    XLOGF(DFATAL, "DirEntry name {} is invalid, should never happen!!!", name);
    co_return makeError(MetaCode::kFoundBug, fmt::format("Invalid DirEntry name {}!", name));
  }
  if (UNLIKELY(name.size() > NAME_MAX)) {
    co_return makeError(MetaCode::kNameTooLong, fmt::format("name {} len > {}", name, NAME_MAX));
  }
  if (UNLIKELY(id.isTreeRoot())) {
    XLOGF(DFATAL, "DirEntry {} points to tree root, should never happen!!!", *this);
    co_return makeError(MetaCode::kFoundBug, fmt::format("DirEntry {} points to tree root", *this));
  }

  auto key = packKey();
  auto value = serde::serialize(data());
  if (auto result = co_await txn.set(key, value); result.hasError()) {
    XLOGF(ERR, "Failed to store dirEntry {}, error {}", *this, result.error());
    co_return result;
  }
  XLOGF(DBG, "DirEntry store {}/{}, {}", parent, name, id);
  co_return Void{};
}

CoTryTask<void> DirEntry::remove(IReadWriteTransaction &txn, bool ignoreSnapshotCheck) const {
  assert(!snapshotLoaded_ || ignoreSnapshotCheck);
  boost::ignore_unused(ignoreSnapshotCheck);
  if (UNLIKELY(!checkName(name))) {
    XLOGF(DFATAL, "DirEntry name {} is invalid, should never happen!!!", name);
    co_return makeError(StatusCode::kInvalidArg, fmt::format("Invalid DirEntry name {}!", name));
  }
  XLOGF(DBG, "Remove direntry {}/{}", parent, name);
  co_return co_await txn.clear(packKey());
}

static inline Result<Inode> checkInodeExists(const DirEntry &entry, std::optional<Inode> result) {
  if (result.has_value()) {
    return std::move(result.value());
  } else if (entry.name == "." || entry.name == "..") {
    // this is a fake dirEntry, so inode may not exists
    XLOGF(DBG, "Inode of entry {} doesn't exist", entry);
    return makeError(MetaCode::kNotFound);
  }
  auto msg = fmt::format("DirEntry {} exists, but Inode not found", entry);
  XLOGF(CRITICAL, "Metadata inconsistent: {}!!!", msg);
  return makeError(MetaCode::kInconsistent, std::move(msg));
}

static inline Result<Inode> checkInodeType(const DirEntry &entry, Inode inode) {
  if (UNLIKELY(inode.getType() != entry.type)) {
    auto msg = fmt::format("DirEntry {}/{} -> {} found, but InodeType mismatch {} != {}",
                           entry.parent,
                           entry.name,
                           entry.id,
                           magic_enum::enum_name(inode.getType()),
                           magic_enum::enum_name(entry.type));
    XLOGF(CRITICAL, "Metadata inconsistent: {}!!!", msg);
    return makeError(MetaCode::kInconsistent, std::move(msg));
  }
  return std::move(inode);
}

template <typename Txn, CoTryTask<std::optional<Inode>> (*LoadFunc)(Txn &, InodeId id)>
static CoTryTask<Inode> loadInodeFromDirEntry(Txn &txn, const DirEntry &entry) {
  co_return (co_await (*LoadFunc)(txn, entry.id))
      .then(folly::partial(checkInodeExists, entry))
      .then(folly::partial(checkInodeType, entry));
}

CoTryTask<Inode> DirEntry::loadInode(IReadOnlyTransaction &txn) const {
  co_return co_await loadInodeFromDirEntry<IReadOnlyTransaction, &Inode::load>(txn, *this);
}

CoTryTask<Inode> DirEntry::snapshotLoadInode(IReadOnlyTransaction &txn) const {
  co_return co_await loadInodeFromDirEntry<IReadOnlyTransaction, &Inode::snapshotLoad>(txn, *this);
}

/** DirEntryList */
template <const bool SNAPSHOT>
CoTryTask<DirEntryList> DirEntryList::loadImpl(IReadOnlyTransaction &txn,
                                               InodeId parent,
                                               IReadOnlyTransaction::KeySelector begin,
                                               IReadOnlyTransaction::KeySelector end,
                                               int32_t limit,
                                               bool loadInodes,
                                               size_t loadInodesConcurrent) {
  auto func = SNAPSHOT ? &IReadOnlyTransaction::snapshotGetRange : &IReadOnlyTransaction::getRange;
  auto result = co_await (txn.*func)(begin, end, limit > 0 ? limit : 128);
  CO_RETURN_ON_ERROR(result);
  bool more = result->hasMore;

  std::vector<DirEntry> entries;
  for (auto &kv : result->kvs) {
    const auto &[key, value] = kv.pair();
    auto entry = DirEntry::newUnpacked(key, value);
    CO_RETURN_ON_ERROR(entry);
    XLOGF_IF(FATAL, entry->parent != parent, "DirEntryList::load {}, get entry {}", parent, *entry);
#ifndef NDEBUG
    entry->snapshotLoaded_ = SNAPSHOT;
#endif
    entries.push_back(std::move(entry.value()));
  }

  if (!loadInodes) {
    co_return DirEntryList{std::move(entries), {}, more};
  }
  if (loadInodesConcurrent <= 0) {
    loadInodesConcurrent = 8;
  }

  auto exec = co_await folly::coro::co_current_executor;
  std::vector<Inode> inodes;
  auto iter = entries.begin();
  while (iter != entries.end()) {
    std::vector<folly::SemiFuture<Result<Inode>>> tasks;
    while (iter != entries.end() && tasks.size() < loadInodesConcurrent) {
      auto &entry = *iter;
      auto func = SNAPSHOT ? &DirEntry::snapshotLoadInode : &DirEntry::loadInode;
      tasks.push_back((entry.*func)(txn).scheduleOn(exec).start());
      iter++;
    }
    auto results = co_await folly::coro::collectAllRange(std::move(tasks));
    for (auto result : results) {
      XLOGF_IF(INFO, result.hasError(), "here error {}", result.error());
      CO_RETURN_ON_ERROR(result);
      inodes.push_back(*result);
    }
  }

  co_return DirEntryList{std::move(entries), std::move(inodes), more};
}

CoTryTask<DirEntryList> DirEntryList::snapshotLoad(IReadOnlyTransaction &txn,
                                                   InodeId parent,
                                                   std::string_view prev,
                                                   int32_t limit,
                                                   bool loadInodes,
                                                   size_t loadInodesConcurrent) {
  std::string beginKey = DirEntry::packKey(parent, prev);
  std::string prefix = DirEntry::packKey(parent, "");
  std::string endKey = kv::TransactionHelper::prefixListEndKey(prefix);
  IReadOnlyTransaction::KeySelector begin{beginKey, false};
  IReadOnlyTransaction::KeySelector end{endKey, false};
  co_return co_await loadImpl<true>(txn, parent, begin, end, limit, loadInodes, loadInodesConcurrent);
}

CoTryTask<DirEntryList> DirEntryList::snapshotLoad(IReadOnlyTransaction &txn,
                                                   InodeId parent,
                                                   std::string_view begin,
                                                   std::string_view end,
                                                   int32_t limit,
                                                   bool loadInodes,
                                                   size_t loadInodesConcurrent) {
  std::string beginKey = DirEntry::packKey(parent, begin);
  std::string endKey = DirEntry::packKey(parent, end);
  IReadOnlyTransaction::KeySelector selBegin{beginKey, false};
  IReadOnlyTransaction::KeySelector selEnd{endKey, false};
  co_return co_await loadImpl<false>(txn, parent, selBegin, selEnd, limit, loadInodes, loadInodesConcurrent);
}

CoTryTask<DirEntryList> DirEntryList::load(IReadWriteTransaction &txn,
                                           InodeId parent,
                                           std::string_view prev,
                                           int32_t limit) {
  std::string beginKey = DirEntry::packKey(parent, prev);
  std::string prefix = DirEntry::packKey(parent, "");
  std::string endKey = kv::TransactionHelper::prefixListEndKey(prefix);
  IReadOnlyTransaction::KeySelector begin{beginKey, false};
  IReadOnlyTransaction::KeySelector end{endKey, false};
  co_return co_await loadImpl<false>(txn, parent, begin, end, limit, false, 8);
}

CoTryTask<bool> DirEntryList::checkEmpty(IReadWriteTransaction &txn, InodeId parent) {
  auto prefix = DirEntry::packKey(parent, "");
  auto endKey = kv::TransactionHelper::prefixListEndKey(prefix);
  IReadWriteTransaction::KeySelector begin(prefix, false);
  IReadWriteTransaction::KeySelector end(endKey, false);
  auto result = co_await txn.getRange(begin, end, 1);
  CO_RETURN_ON_ERROR(result);

  co_return result->kvs.empty() && !result->hasMore;
}

}  // namespace hf3fs::meta::server
