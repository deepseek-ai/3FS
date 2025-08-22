#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fmt/core.h>
#include <folly/Expected.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {
using hf3fs::kv::IReadOnlyTransaction;
using hf3fs::kv::IReadWriteTransaction;

struct DirEntryList;

class DirEntry : public meta::DirEntry {
 public:
  using Base = meta::DirEntry;
  using Base::Base;

  DirEntry(Base base)
      : Base(std::move(base)) {}

  static Result<DirEntry> newUnpacked(const std::string_view key, const std::string_view value);

  static CoTryTask<std::optional<DirEntry>> snapshotLoad(IReadOnlyTransaction &txn,
                                                         InodeId parent,
                                                         std::string_view name);

  static CoTryTask<std::optional<DirEntry>> load(IReadOnlyTransaction &txn, InodeId parent, std::string_view name);

  static CoTryTask<bool> checkExist(IReadOnlyTransaction &txn, InodeId parent, std::string_view name) {
    co_return (co_await DirEntry::load(txn, parent, name)).then([](auto &v) { return v.has_value(); });
  }

  static DirEntry newFile(InodeId parent, std::string name, InodeId inode) {
    return meta::DirEntry(parent, name, {inode, InodeType::File});
  }
  static DirEntry newSymlink(InodeId parent, std::string name, InodeId inode) {
    return meta::DirEntry(parent, name, {inode, InodeType::Symlink});
  }
  static DirEntry newDirectory(InodeId parent, std::string name, InodeId inode, Acl acl) {
    return meta::DirEntry(parent, name, {inode, InodeType::Directory, acl});
  }
  static DirEntry root() {
    return meta::DirEntry(InodeId::root(), ".", {InodeId::root(), InodeType::Directory, Acl::root()});
  }

  /** Key format: prefix + parent-InodeId.key + name */
  std::string packKey() const;
  static std::string packKey(InodeId parent, std::string_view name);
  Result<Void> unpackKey(const std::string_view key);

  // load inode from dir entry
  CoTryTask<Inode> loadInode(IReadOnlyTransaction &txn) const;
  CoTryTask<Inode> snapshotLoadInode(IReadOnlyTransaction &txn) const;
  CoTryTask<void> addIntoReadConflict(IReadWriteTransaction &txn) const {
#ifndef NDEBUG
    snapshotLoaded_ = false;
#endif
    co_return co_await txn.addReadConflict(packKey());
  }
  CoTryTask<void> store(IReadWriteTransaction &txn) const;
  CoTryTask<void> remove(IReadWriteTransaction &txn, bool ignoreSnapshotCheck = false) const;

 private:
  friend struct DirEntryList;
  friend class MetaTestHelper;

  template <const bool SNAPSHOT>
  static CoTryTask<std::optional<DirEntry>> loadImpl(IReadOnlyTransaction &txn, InodeId parent, std::string_view name);

#ifndef NDEBUG
  mutable bool snapshotLoaded_ = false;
#endif
};

struct DirEntryList {
  std::vector<DirEntry> entries;
  std::vector<Inode> inodes;
  bool more;

  // (prev, end)
  static CoTryTask<DirEntryList> snapshotLoad(IReadOnlyTransaction &txn,
                                              InodeId parent,
                                              std::string_view prev,
                                              int32_t limit,
                                              bool loadInodes = false,
                                              size_t loadInodesConcurrent = 0);

  // (begin, end)
  static CoTryTask<DirEntryList> snapshotLoad(IReadOnlyTransaction &txn,
                                              InodeId parent,
                                              std::string_view begin,
                                              std::string_view end,
                                              int32_t limit,
                                              bool loadInodes = false,
                                              size_t loadInodesConcurrent = 0);

  static CoTryTask<DirEntryList> load(IReadWriteTransaction &txn, InodeId parent, std::string_view prev, int32_t limit);

  static CoTryTask<bool> checkEmpty(IReadWriteTransaction &txn, InodeId parent);

  // For recursive remove and move to the trash, permission checks are required.
  // However, because the directory may be very large, we may not able to check permissions for entire
  // directory tree. This method is best effort.
  static CoTryTask<Void> recursiveCheckRmPerm(IReadWriteTransaction &txn,
                                              InodeId parent,
                                              flat::UserInfo user,
                                              int32_t limit,
                                              size_t listBatchSize) {
    static monitor::CountRecorder failed("meta_server.recursive_check_rm_perm_failed");
    auto guard = folly::makeGuard([&]() {
      failed.addSample(1, {{"uid", folly::to<std::string>(user.uid.toUnderType())}});
    });

    auto queue = std::queue<InodeId>();
    queue.push(parent);
    while (!queue.empty()) {
      auto currDir = queue.front();
      queue.pop();

      auto prev = std::string();
      auto foundDir = false;
      auto numEntries = 0;
      while (true) {
        if (limit-- <= 0) {
          break;
        }
        auto list = co_await DirEntryList::snapshotLoad(txn, currDir, prev, std::max(listBatchSize, 32ul));
        CO_RETURN_ON_ERROR(list);
        numEntries += list->entries.size();
        for (auto &entry : list->entries) {
          prev = entry.name;
          if (!entry.isDirectory()) {
            continue;
          }
          foundDir = true;
          if ((int64_t)queue.size() < limit) {
            queue.push(entry.id);
          }
          auto &acl = *entry.dirAcl;
          if (auto res = acl.checkRecursiveRmPerm(user, false); res.hasError()) {
            auto msg = fmt::format("user {} recursive remove {}, found {} without permission, msg {}",
                                   user.uid,
                                   parent,
                                   entry,
                                   res.error().message());
            XLOG(ERR, msg);
            co_return makeError(MetaCode::kNoPermission, msg);
          }
        }
        if (!list->more || (numEntries > 1024 && !foundDir)) {
          break;
        }
      }
    }
    guard.dismiss();
    co_return Void{};
  }

  DirEntry &entry(size_t i) { return entries.at(i); }
  const DirEntry &entry(size_t i) const { return entries.at(i); }

  const Inode &inode(size_t i) const { return inodes.at(i); }
  Inode &inode(size_t i) { return inodes.at(i); }

  operator ListRsp() && {
    ListRsp rsp;
    rsp.more = more;
    rsp.entries.reserve(entries.size());
    rsp.inodes.reserve(inodes.size());
    for (auto &entry : entries) {
      rsp.entries.emplace_back(std::move(entry));
    }
    for (auto &inode : inodes) {
      rsp.inodes.emplace_back(std::move(inode));
    }
    return rsp;
  }

 private:
  template <const bool SNAPSHOT>
  static CoTryTask<DirEntryList> loadImpl(IReadOnlyTransaction &txn,
                                          InodeId parent,
                                          IReadOnlyTransaction::KeySelector begin,
                                          IReadOnlyTransaction::KeySelector end,
                                          int32_t limit,
                                          bool loadInodes,
                                          size_t loadInodesConcurrent);
};

}  // namespace hf3fs::meta::server
