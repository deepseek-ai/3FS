#pragma once

#include <folly/Overload.h>
#include <folly/Utility.h>
#include <gtest/gtest_prod.h>
#include <optional>
#include <utility>
#include <variant>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Path.h"
#include "fbs/meta/Common.h"
#include "meta/components/AclCache.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

/**
 * Path Resolution.
 *
 * Note: PathResolveOp always use snapshotLoad, so it won't add any key into read conflict set.
 * User should add keys into read conflict set manually if needed.
 */
class PathResolveOp : folly::NonCopyableNonMovable {
 public:
  struct ResolveResult {
    // for parent, we may already got it's Inode, or just dirEntry points to it, or just cached acl
    std::variant<std::pair<InodeId, Acl>, Inode, DirEntry> parent;
    std::optional<DirEntry> dirEntry;

    ResolveResult(std::variant<std::pair<InodeId, Acl>, Inode, DirEntry> parent, std::optional<DirEntry> dirEntry)
        : parent(std::move(parent)),
          dirEntry(std::move(dirEntry)) {}

    InodeId getParentId() const { return getInodeId(parent); }
    Acl getParentAcl() const { return getDirectoryAcl(parent); }
    CoTryTask<Inode> getParentInode(kv::IReadOnlyTransaction &txn) const {
      if (std::holds_alternative<Inode>(parent)) {
        co_return std::get<Inode>(parent);
      } else if (std::holds_alternative<DirEntry>(parent)) {
        co_return co_await std::get<DirEntry>(parent).snapshotLoadInode(txn);
      } else {
        auto parentId = std::get<std::pair<InodeId, Acl>>(parent).first;
        co_return (co_await Inode::snapshotLoad(txn, parentId)).then(checkMetaFound<Inode>);
      }
    }
  };

  struct ResolveRangeResult : ResolveResult {
    Path missing;
    ResolveRangeResult(ResolveResult result, Path missing)
        : ResolveResult(std::move(result)),
          missing(missing) {}
  };

  PathResolveOp(IReadOnlyTransaction &txn, AclCache &aclCache, const UserInfo &userInfo, Path *trace = nullptr)
      : PathResolveOp(txn, aclCache, userInfo, trace, 4, 8, 5_s) {}
  PathResolveOp(IReadOnlyTransaction &txn,
                AclCache &aclCache,
                const UserInfo &userInfo,
                Path *trace,
                size_t maxSymlinkCount,
                size_t maxSymlinkDepth,
                Duration aclCacheTime)
      : txn_(txn),
        user_(userInfo),
        aclCache_(aclCache),
        trace_(trace),
        depth_(0),
        symlinkCnt_(0),
        maxSymlinkCount_(maxSymlinkCount),
        maxSymlinkDepth_(maxSymlinkDepth),
        aclCacheTime_(aclCacheTime),
        pathComponents_(0) {}
  ~PathResolveOp();

  CoTryTask<Inode> inode(const PathAt &path, AtFlags flags, bool checkRefCnt);
  CoTryTask<DirEntry> dirEntry(const PathAt &path, AtFlags flags);

  CoTryTask<ResolveResult> path(const PathAt &path, AtFlags flags);
  CoTryTask<ResolveResult> byDirectoryInodeId(InodeId inodeId);
  CoTryTask<ResolveRangeResult> pathRange(const PathAt &path);

  CoTryTask<ResolveResult> symlink(DirEntry entry);

 private:
  template <typename>
  FRIEND_TEST(TestResolve, ResolveComponent);

  CoTryTask<DirEntry> dirEntry(InodeId parent, const Path &path, bool followLastSymlink);

  CoTryTask<ResolveResult> path(InodeId parent, const Path &path);
  CoTryTask<ResolveResult> path(InodeId parent, const Path &path, bool followLastSymlink);
  CoTryTask<ResolveResult> pathComponent(const std::variant<InodeId, DirEntry> &parent, const Path &name);
  CoTryTask<ResolveResult> pathRange(InodeId parent, Path::const_iterator &begin, const Path::const_iterator &end);

  IReadOnlyTransaction &txn_;
  const UserInfo &user_;
  AclCache &aclCache_;
  Path *trace_;

  size_t depth_;
  size_t symlinkCnt_;

  size_t maxSymlinkCount_;
  size_t maxSymlinkDepth_;
  Duration aclCacheTime_;

  size_t pathComponents_;
};

}  // namespace hf3fs::meta::server