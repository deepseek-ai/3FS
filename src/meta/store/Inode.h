#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <folly/Likely.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::meta::server {
using hf3fs::kv::IReadOnlyTransaction;
using hf3fs::kv::IReadWriteTransaction;

class Inode : public meta::Inode {
 public:
  using Base = meta::Inode;
  using Base::Base;

  Inode(Base base)
      : Base(std::move(base)) {}
  Inode(InodeId id, Acl acl, UtcTime time, std::variant<File, Directory, Symlink> type)
      : Base{id, InodeData{type, acl, 1, time, time, time}} {}

  static Inode newFile(InodeId id, Acl acl, Layout layout, UtcTime time) { return Inode(id, acl, time, File(layout)); }

  static Inode newDirectory(InodeId id, InodeId parent, std::string name, Acl acl, Layout layout, UtcTime time) {
    return Inode(id, acl, time, Directory{parent, std::move(layout), std::move(name)});
  }

  static Inode newSymlink(InodeId id, Path target, Uid uid, Gid gid, UtcTime time) {
    static constexpr Permission perm{0777};  // permission of symlink is never used, and won't changed
    return Inode(id, Acl(uid, gid, perm), time, Symlink{std::move(target)});
  }

  /** key format: kInodePrefx + InodeId.key */
  static std::string packKey(InodeId id);
  std::string packKey() const;
  Result<Void> unpackKey(std::string_view key);

  static Result<Inode> newUnpacked(std::string_view key, std::string_view value);

  // The difference of `snapshotLoad` and `load` is the former won't add key of inode into read conflict set.
  static CoTryTask<std::optional<Inode>> snapshotLoad(IReadOnlyTransaction &txn, InodeId id);
  static CoTryTask<std::optional<Inode>> load(IReadOnlyTransaction &txn, InodeId id);

  CoTryTask<void> addIntoReadConflict(IReadWriteTransaction &txn) {
#ifndef NDEBUG
    snapshotLoaded_ = false;
#endif
    co_return co_await txn.addReadConflict(packKey());
  }

  CoTryTask<void> store(IReadWriteTransaction &txn) const;
  /** Remove this inode */
  CoTryTask<void> remove(IReadWriteTransaction &txn) const;

  CoTryTask<DirEntry> snapshotLoadDirEntry(IReadOnlyTransaction &txn) const;

  static CoTryTask<Void> loadAncestors(IReadWriteTransaction &txn, std::vector<Inode> &ancestors, InodeId parent);

 private:
  template <const bool SNAPSHOT>
  static CoTryTask<std::optional<Inode>> loadImpl(IReadOnlyTransaction &txn, InodeId id);

#ifndef NDEBUG
  mutable bool snapshotLoaded_ = false;
#endif
};

static_assert(serde::SerializableToJson<Inode>);
}  // namespace hf3fs::meta::server
