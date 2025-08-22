#include "meta/store/Inode.h"

#include <cassert>
#include <cstddef>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <linux/fs.h>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "meta/store/BatchContext.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

/** Inode */
Result<Inode> Inode::newUnpacked(std::string_view key, std::string_view value) {
  Inode inode;
  RETURN_ON_ERROR(inode.unpackKey(key));
  if (auto result = serde::deserialize(inode.data(), value); result.hasError()) {
    XLOGF(CRITICAL, "Failed to deserialize inode value {}, data corruption!", result.error());
    return makeError(StatusCode::kDataCorruption);
  }
  return std::move(inode);
}

std::string Inode::packKey(InodeId id) {
  static constexpr auto prefix = kv::KeyPrefix::Inode;
  auto inodeId = id.packKey();
  return Serializer::serRawArgs(prefix, inodeId);
}

std::string Inode::packKey() const { return packKey(id); }

Result<Void> Inode::unpackKey(std::string_view key) {
  kv::KeyPrefix prefix;
  InodeId::Key inodeId;
  if (auto result = Deserializer::deserRawArgs(key, prefix, inodeId); result.hasError()) {
    XLOGF(CRITICAL, "Failed to deserialize inode key {}, data corruption!", result.error());
    return makeError(StatusCode::kDataCorruption);
  }
  assert(prefix == kv::KeyPrefix::Inode);
  id = InodeId::unpackKey(inodeId);

  return Void{};
}

template <const bool SNAPSHOT>
CoTryTask<std::optional<Inode>> Inode::loadImpl(IReadOnlyTransaction &txn, InodeId id) {
  auto func = SNAPSHOT ? &IReadOnlyTransaction::snapshotGet : &IReadOnlyTransaction::get;
  auto result = co_await (txn.*func)(packKey(id));
  if (result.hasError()) {
    XLOGF(ERR, "Failed to load inode {}, error {}", id, result.error());
    CO_RETURN_ERROR(result);
  }
  if (auto &value = *result; value.has_value()) {
    Inode inode;
    inode.id = id;
#ifndef NDEBUG
    inode.snapshotLoaded_ = SNAPSHOT;
#endif
    if (auto des = serde::deserialize(inode.data(), *value); des.hasError()) {
      XLOGF(CRITICAL, "Failed to deserialize inode {}, {}, data corruption!!", inode.id, des.error());
      co_return makeError(StatusCode::kDataCorruption, fmt::format("deserialize inode {} failed", id));
    }
    co_return std::move(inode);
  } else {
    co_return std::nullopt;
  }
}

CoTryTask<std::optional<Inode>> Inode::snapshotLoad(IReadOnlyTransaction &txn, InodeId id) {
  if (auto batch = BatchContext::get(); batch) {
    auto guard = batch->loadInode(id);
    if (guard.needLoad) {
      auto v = co_await loadImpl<true>(txn, id);
      guard.set(v);
      co_return std::move(v);
    } else {
      co_return co_await guard.coAwait();
    }
  } else {
    co_return co_await loadImpl<true>(txn, id);
  }
}

CoTryTask<std::optional<Inode>> Inode::load(IReadOnlyTransaction &txn, InodeId id) {
  co_return co_await loadImpl<false>(txn, id);
}

CoTryTask<void> Inode::store(IReadWriteTransaction &txn) const {
  static const std::map<InodeId, Acl> treeRoots{{InodeId::root(), Acl::root()}, {InodeId::gcRoot(), Acl::gcRoot()}};
  assert(!snapshotLoaded_);
  if (treeRoots.contains(id)) {
    if (!isDirectory() || (!asDirectory().name.empty() && asDirectory().name != "/")) {
      XLOGF(DFATAL, "Store invalid root inode, {}", *this);
      co_return makeError(MetaCode::kFoundBug, fmt::format("Store invalid special inode {}", *this));
    }
    auto expectedAcl = treeRoots.at(id);
    expectedAcl.iflags = acl.iflags;
    if (acl != expectedAcl) {
      XLOGF(DFATAL, "Try change root inode {} acl to {}", id, acl);
      co_return makeError(MetaCode::kNoPermission, fmt::format("try change root {} acl to {}", id, acl));
    }
  } else if (isDirectory()) {
    auto &name = asDirectory().name;
    if (UNLIKELY(name == "." || name == ".." || std::find(name.begin(), name.end(), '/') != name.end())) {
      XLOGF(DFATAL, "DirEntry name {} is invalid, should never happen!!!", name);
      co_return makeError(MetaCode::kFoundBug, fmt::format("Directory {} invalid DirEntry name {}", id, name));
    }
  } else if (isFile()) {
    if (auto valid = asFile().layout.valid(false /*allowEmpty*/); valid.hasError()) {
      XLOGF(DFATAL, "File {} has a invalid layout {}, error {}", id, asFile().layout, valid.error());
      co_return makeError(MetaCode::kFoundBug,
                          fmt::format("File {} invalid layout {}, {}", id, asFile().layout, valid.error()));
    }
  }

  auto key = packKey();
  auto value = serde::serialize(data());
  if (auto result = co_await txn.set(key, value); result.hasError()) {
    XLOGF(ERR, "Failed to store inode {}, error {}", id, result.error());
    co_return result;
  }
  XLOGF(DBG, "Store inode {}", id);
  co_return Void{};
}

CoTryTask<void> Inode::remove(IReadWriteTransaction &txn) const {
  assert(!snapshotLoaded_);
  if (UNLIKELY(id.isTreeRoot())) {
    XLOGF(DFATAL, "Don't allow remove tree root {}!", id);
    co_return makeError(MetaCode::kFoundBug, "Try remove tree root");
  }
  if (UNLIKELY(acl.iflags & FS_IMMUTABLE_FL)) {
    XLOGF(DFATAL, "Try remove inode {} with FS_IMMUTABLE_FL", id);
    co_return makeError(MetaCode::kFoundBug, "Try remove inode with FS_IMMUTABLE_FL");
  }
  XLOGF(DBG, "Remove inode {}", id);
  co_return co_await txn.clear(packKey());
}

CoTryTask<meta::DirEntry> Inode::snapshotLoadDirEntry(IReadOnlyTransaction &txn) const {
  if (!isDirectory()) {
    co_return makeError(MetaCode::kNotDirectory);
  }

  auto parent = asDirectory().parent;
  auto name = asDirectory().name;
  std::optional<DirEntry> entry;
  if (!name.empty()) {
    auto result = co_await DirEntry::snapshotLoad(txn, parent, name);
    CO_RETURN_ON_ERROR(result);
    entry = std::move(*result);
  } else {
    std::string prev;
    while (true) {
      auto entries = co_await DirEntryList::snapshotLoad(txn, parent, prev, -1, false);
      CO_RETURN_ON_ERROR(entries);
      for (auto &item : entries->entries) {
        if (item.id == id) {
          entry = std::move(item);
          break;
        }
      }
      if (!entries->more) break;
      if (!entries->entries.empty()) prev = entries->entries.rbegin()->name;
    }
  }

  if (!entry.has_value()) {
    XLOGF(WARN, "DirEntry of directory {} not found, parent {}, path {}, maybe deleted!", id, parent, name);
    co_return makeError(MetaCode::kNotFound);
  } else if (entry->id != id) {
    XLOGF(WARN, "InodeId of DirEntry {} != {}, maybe deleted", *entry, id);
    co_return makeError(MetaCode::kNotFound);
  }

  co_return *entry;
}

CoTryTask<Void> Inode::loadAncestors(IReadWriteTransaction &txn, std::vector<Inode> &ancestors, InodeId parent) {
  auto ancestorIds = std::set<InodeId>();
  auto currAncestorId = parent;
  FAULT_INJECTION_SET_FACTOR(4);
  while (true) {
    if (UNLIKELY(ancestorIds.contains(currAncestorId))) {
      XLOGF(DFATAL, "Inode found duplicated ancestor, parent {}, duplicated {}", parent, currAncestorId);
      co_return makeError(MetaCode::kInconsistent, "directory tree contains loop");
    }
    ancestorIds.insert(currAncestorId);

    // NOTE: add dst's ancestors inode into read conflict set
    auto currAncestor = (co_await Inode::load(txn, currAncestorId)).then(checkMetaFound<Inode>);
    CO_RETURN_ON_ERROR(currAncestor);
    ancestors.push_back(*currAncestor);
    if (UNLIKELY(!currAncestor->isDirectory())) {
      XLOGF(DFATAL, "Entry {}, Inode {} is not directory", currAncestorId, *currAncestor);
      co_return makeError(MetaCode::kNotDirectory);
    }

    if (currAncestor->asDirectory().parent == currAncestor->id) {
      break;
    }

    currAncestorId = currAncestor->asDirectory().parent;
  }

  co_return Void{};
}

}  // namespace hf3fs::meta::server
