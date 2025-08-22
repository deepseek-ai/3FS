#include "meta/store/PathResolve.h"

#include <cstddef>
#include <fcntl.h>
#include <folly/Overload.h>
#include <folly/ScopeGuard.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <variant>

#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "meta/components/AclCache.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

namespace {
monitor::DistributionRecorder pathComponentsDist("meta_server.path_components");
monitor::DistributionRecorder pathComponentsDistUser("meta_server.path_components_by_user");
}  // namespace

using ResolveResult = PathResolveOp::ResolveResult;
using ResolveRangeResult = PathResolveOp::ResolveRangeResult;

static CoTryTask<std::variant<std::pair<InodeId, Acl>, Inode, DirEntry>> loadParentAcl(
    IReadOnlyTransaction &txn,
    AclCache &cache,
    const UserInfo &user,
    const std::variant<InodeId, DirEntry> &parent,
    Duration cacheTime) {
  auto parentId = getInodeId(parent);
  std::variant<std::pair<InodeId, Acl>, Inode, DirEntry> parentInfo;
  if (std::holds_alternative<DirEntry>(parent)) {
    // we already have Acl in DirEntry
    parentInfo = std::get<DirEntry>(parent);
    if (cacheTime.count() != 0 && std::get<DirEntry>(parent).isDirectory()) {
      cache.set(parentId, getDirectoryAcl(parentInfo));
    }
  } else {
    if (parentId == InodeId::root()) {
      parentInfo = DirEntry::root();
    } else {
      auto cached = cache.get(parentId, cacheTime);
      if (cached.has_value()) {
        parentInfo = std::pair<InodeId, Acl>{parentId, *cached};
      } else {
        auto loadResult = (co_await Inode::snapshotLoad(txn, parentId)).then(checkMetaFound<Inode>);
        CO_RETURN_ON_ERROR(loadResult);
        if (cacheTime.count() != 0 && loadResult->isDirectory()) {
          cache.set(parentId, loadResult->acl);
        }
        parentInfo = std::move(*loadResult);
      }
    }
  }

  if (getInodeType(parentInfo) != InodeType::Directory) {
    co_return makeError(MetaCode::kNotDirectory);
  }
  co_return parentInfo;
}

static CoTryTask<std::variant<std::pair<InodeId, Acl>, Inode, DirEntry>> loadAndCheckParentAcl(
    IReadOnlyTransaction &txn,
    AclCache &cache,
    const UserInfo &user,
    const std::variant<InodeId, DirEntry> &parent,
    Duration cacheTime) {
  auto acl = co_await loadParentAcl(txn, cache, user, parent, cacheTime);
  CO_RETURN_ON_ERROR(acl);
  CO_RETURN_ON_ERROR(getDirectoryAcl(*acl).checkPermission(user, AccessType::EXEC));
  co_return acl;
}

PathResolveOp::~PathResolveOp() {
  if (pathComponents_) {
    pathComponentsDist.addSample(pathComponents_);
    pathComponentsDistUser.addSample(pathComponents_, {{"uid", folly::to<std::string>(user_.uid.toUnderType())}});
  }
}

CoTryTask<Inode> PathResolveOp::inode(const PathAt &path, AtFlags flags, bool checkRefCnt) {
  Result<Inode> inode = makeError(MetaCode::kFoundBug);
  if (!path.path.has_value() || (path.path->empty() && flags.contains(AT_EMPTY_PATH))) {
    inode = (co_await Inode::snapshotLoad(txn_, path.parent)).then(checkMetaFound<Inode>);
  } else {
    auto entry = co_await this->dirEntry(path, flags);
    CO_RETURN_ON_ERROR(entry);
    assert(!flags.followLastSymlink() || !entry->isSymlink());
    inode = co_await entry->snapshotLoadInode(txn_);
    // XLOGF_IF(DFATAL, (inode.hasValue() && inode->nlink == 0), "entry {} -> inode {}, nlink == 0", *entry, *inode);
  }

  if (!inode.hasError() && checkRefCnt && inode->nlink == 0) {
    co_return makeError(MetaCode::kNotFound,
                        fmt::format("path {}, inode {} is removed", path, inode->id.toHexString()));
  }
  co_return inode;
}

CoTryTask<DirEntry> PathResolveOp::dirEntry(const PathAt &path, AtFlags flags) {
  if (!path.path.has_value()) {
    co_return makeError(StatusCode::kInvalidArg, "path not set");
  } else {
    co_return co_await dirEntry(path.parent, *path.path, flags.followLastSymlink());
  }
}

CoTryTask<DirEntry> PathResolveOp::dirEntry(InodeId parent, const Path &path, bool followLastSymlink) {
  auto resolveResult = co_await this->path(parent, path);
  CO_RETURN_ON_ERROR(resolveResult);

  if (auto &entry = resolveResult->dirEntry; entry.has_value() && entry->isSymlink() && followLastSymlink) {
    XLOGF(DBG, "Resolve dir entry get symlink, follow it.");
    resolveResult = co_await this->symlink(*entry);
    CO_RETURN_ON_ERROR(resolveResult);
  }

  if (!resolveResult->dirEntry.has_value()) {
    co_return makeError(MetaCode::kNotFound);
  }

  co_return std::move(*resolveResult->dirEntry);
}

CoTryTask<ResolveResult> PathResolveOp::byDirectoryInodeId(InodeId inodeId) {
  auto inode = (co_await Inode::snapshotLoad(txn_, inodeId)).then(checkMetaFound<Inode>);
  CO_RETURN_ON_ERROR(inode);
  auto entry = co_await inode->snapshotLoadDirEntry(txn_);
  CO_RETURN_ON_ERROR(entry);
  auto parentAcl = co_await loadParentAcl(txn_, aclCache_, user_, entry->parent, aclCacheTime_);
  CO_RETURN_ON_ERROR(parentAcl);
  co_return ResolveResult(*parentAcl, *entry);
}

CoTryTask<ResolveResult> PathResolveOp::path(const PathAt &path, AtFlags flags) {
  if (!path.path.has_value()) {
    co_return makeError(StatusCode::kInvalidArg, "path not set");
  } else {
    co_return co_await this->path(path.parent, *path.path, flags.followLastSymlink());
  }
}

/**
 * Resolve path, return parentInode of last path component and dirEntry if presents.
 * Don't follow last symlink link.
 */
CoTryTask<ResolveResult> PathResolveOp::path(InodeId parent, const Path &path) {
  XLOGF(DBG, "Resolve path {}/{}", parent, path);
  auto begin = path.begin();
  auto resolveResult = co_await this->pathRange(parent, begin, path.end());
  CO_RETURN_ON_ERROR(resolveResult);

  XLOGF(DBG,
        "Resolve path {}, {} components, {} found, {} missing",
        path,
        std::distance(path.begin(), path.end()),
        std::distance(path.begin(), begin),
        std::distance(begin, path.end()));

  if (begin == path.end() || ++begin == path.end()) {
    co_return resolveResult;
  } else {
    // some middle path components missing, return kNotFound
    co_return makeError(MetaCode::kNotFound);
  }
}

CoTryTask<ResolveResult> PathResolveOp::path(InodeId parent, const Path &path, bool followLastSymlink) {
  auto resolveResult = co_await this->path(parent, path);
  CO_RETURN_ON_ERROR(resolveResult);
  if (auto &entry = resolveResult->dirEntry; followLastSymlink && entry.has_value() && entry->isSymlink()) {
    co_return co_await symlink(*entry);
  }
  co_return resolveResult;
}

CoTryTask<ResolveRangeResult> PathResolveOp::pathRange(const PathAt &path) {
  if (!path.path.has_value()) {
    co_return makeError(StatusCode::kInvalidArg, "path not set");
  } else {
    auto begin = path.path->begin();
    auto end = path.path->end();
    auto result = co_await pathRange(path.parent, begin, end);
    CO_RETURN_ON_ERROR(result);
    co_return ResolveRangeResult(std::move(*result), std::accumulate(begin, end, Path(), std::divides()));
  }
}

/**
 * Walk along and resolve path components from begin to end.
 * If parent doesn't exists or path is empty, return kNotFound.
 * If parent is symlink, return kNotDirectory, if any middle path component points to a symlink, try to resolve it.
 * If parent or any middle path component is symlink that points to a non-exist path, return kNotFound.
 * If parent or any middle path component is a file, return kNotDirectory.
 * If parent or any middle path component points to a deleted directory, return kNotFound.
 * If user does not have search permission on directory, return kNoPermission.
 * If there is too much symlink during path resolution, return kTooManySymlinks.
 * If any path component is missing, begin will points to corresponding path component, and returns its parent Inode.
 * If resolution success, begin will point to end and returns last parent Inode and DirEntry.
 */
CoTryTask<ResolveResult> PathResolveOp::pathRange(InodeId parentId,
                                                  Path::const_iterator &begin,
                                                  const Path::const_iterator &end) {
  SCOPE_EXIT {
    auto dis = std::distance(begin, end);
    XLOGF_IF(DBG,
             dis,
             "PathResolveOp::pathRange {} components missing, {}!",
             dis,
             std::accumulate(begin, end, Path(), std::divides()));
  };
  std::variant<InodeId, DirEntry> parent(parentId);
  if (begin == end) {
    co_return makeError(MetaCode::kNotFound);
  }
  if (*begin == "/") {
    // lookup from root;
    parent = InodeId::root();
    if (++begin == end) {
      if (trace_) {
        *trace_ = "/";
      }
      // special case: path range only contains "/", just load root inode and make a fake directory entry
      co_return ResolveResult(DirEntry::root(), DirEntry::root());
    }
  }

  FAULT_INJECTION_SET_FACTOR(std::distance(begin, end));
  while (begin != end) {
    // do not need to handle "."
    if (begin->filename_is_dot()) {
      if (++begin == end) {
        co_return co_await pathComponent(parent, ".");
      }
      continue;
    }
    // resolve current path component
    auto resolveResult = co_await pathComponent(parent, *begin);
    CO_RETURN_ON_ERROR(resolveResult);
    if (!resolveResult->dirEntry.has_value()) {
      // dirEntry not found, just means this path component is missing,
      // return parentInode and let caller decide create missing path components or not
      co_return resolveResult;
    }

    if (++begin == end) {
      // this is last component, return here
      co_return resolveResult;
    }

    // middle path component
    if (resolveResult->dirEntry->isSymlink()) {
      resolveResult = co_await this->symlink(*resolveResult->dirEntry);
      CO_RETURN_ON_ERROR(resolveResult);
      if (!resolveResult->dirEntry.has_value()) {
        co_return makeError(MetaCode::kNotFound);
      }
    }

    if (resolveResult->dirEntry->isFile()) {
      co_return makeError(MetaCode::kNotDirectory);
    }

    // update parent and continue.
    parent = std::move(resolveResult->dirEntry.value());
  }

  __builtin_unreachable();
}

/**
 * Resolve a single path component.
 * If parent doesn't exist, return kNotFound or kInconsistent.
 * If parent exists but is deleted, return kNotFound.
 * If parent is file or symlink, return kNotDirectory.
 * If parent is directory, but user doesn't have search permission, return kNoPermission.
 * Else return parentInode and dirEntry if exists.
 */
CoTryTask<ResolveResult> PathResolveOp::pathComponent(const std::variant<InodeId, DirEntry> &parent, const Path &name) {
  // todo: For each directory, we need load it's Inode to check permission,
  // this adds performance overhead to path resolution.
  // A simple way to mitigate this is cache Inode permission information,
  // if we can tolerate chmod doesn't make effect for several seconds.
  auto parentId = getInodeId(parent);
  if (trace_) {
    if (parentId == InodeId::root()) {
      // todo: many for other root?
      *trace_ = "/";
    }
    *trace_ /= name;
  }

  if (!name.filename_is_dot()) pathComponents_++;

  auto result = co_await loadAndCheckParentAcl(txn_, aclCache_, user_, parent, aclCacheTime_);
  CO_RETURN_ON_ERROR(result);
  if (name.filename_is_dot()) {
    DirEntry dirEntry = DirEntry::newDirectory(parentId, ".", parentId, getDirectoryAcl(*result));
    co_return ResolveResult(*result, dirEntry);
  } else if (name.filename_is_dot_dot()) {
    if (std::holds_alternative<Inode>(*result)) {
      auto &parent = std::get<Inode>(*result);
      auto ppId = parent.asDirectory().parent;
      auto parentAcl = parent.acl;
      co_return ResolveResult(std::move(parent), DirEntry::newDirectory(parentId, "..", ppId, parentAcl));
    } else {
      auto loadInodeResult = (co_await Inode::snapshotLoad(txn_, getInodeId(*result))).then(checkMetaFound<Inode>);
      CO_RETURN_ON_ERROR(loadInodeResult);
      auto &parent = *loadInodeResult;
      auto ppId = parent.asDirectory().parent;
      auto parentAcl = parent.acl;
      co_return ResolveResult(std::move(parent), DirEntry::newDirectory(parentId, "..", ppId, parentAcl));
    }
  } else {
    auto loadEntryResult = co_await DirEntry::snapshotLoad(txn_, parentId, name.native());
    CO_RETURN_ON_ERROR(loadEntryResult);
    co_return ResolveResult(std::move(*result), std::move(loadEntryResult.value()));
  }
}

static CoTryTask<Path> loadSymLinkTarget(IReadOnlyTransaction &txn, const DirEntry &entry) {
  auto symlinkResult = co_await entry.snapshotLoadInode(txn);
  CO_RETURN_ON_ERROR(symlinkResult);
  co_return std::move(symlinkResult.value().asSymlink().target);
}

CoTryTask<ResolveResult> PathResolveOp::symlink(DirEntry entry) {
  if (++depth_ > maxSymlinkDepth_) {
    co_return makeError(MetaCode::kTooManySymlinks);
  }
  SCOPE_EXIT { depth_--; };

  while (true) {
    if (++symlinkCnt_ > maxSymlinkCount_) {
      co_return makeError(MetaCode::kTooManySymlinks);
    }
    auto symlinkTarget = co_await loadSymLinkTarget(txn_, entry);
    CO_RETURN_ON_ERROR(symlinkTarget);
    if (trace_) {
      trace_->remove_filename();
    }
    auto resolveResult = co_await this->path(entry.parent, *symlinkTarget);
    CO_RETURN_ON_ERROR(resolveResult);
    if (!resolveResult->dirEntry.has_value() || !resolveResult->dirEntry->isSymlink()) {
      co_return resolveResult;
    }
    entry = std::move(*resolveResult->dirEntry);
  }
}

}  // namespace hf3fs::meta::server
