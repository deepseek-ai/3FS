#include <algorithm>
#include <cmath>
#include <compare>
#include <folly/ScopeGuard.h>
#include <folly/lang/Ordering.h>
#include <folly/logging/xlog.h>
#include <linux/fs.h>
#include <memory>
#include <optional>
#include <type_traits>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/base/Config.h"
#include "meta/service/MetaOperator.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

class SetAttr {
 public:
  static Result<Void> check(const Inode &inode, const SetAttrReq &req, const Config &config) {
    RETURN_ON_ERROR(req.valid());

    // permission check for setPermission
    if (inode.id.isTreeRoot() && (req.perm || req.uid || req.gid)) {
      XLOGF(WARN, "Don't allow change permission of tree root {}!", inode.id);
      return makeError(MetaCode::kNoPermission, fmt::format("Don't allow change permission of {}", inode.id));
    }
    if (req.iflags.has_value() && *req.iflags != inode.acl.iflags) {
      auto setChainAllocation = !(inode.acl.iflags & FS_CHAIN_ALLOCATION_FL) && (*req.iflags & FS_CHAIN_ALLOCATION_FL);
      if (setChainAllocation && !config.iflags_chain_allocation()) {
        return makeError(MetaCode::kNoPermission, "FS_CHAIN_ALLOCATION_FL disabled");
      }
      auto setNewChunkEngine = !(inode.acl.iflags & FS_NEW_CHUNK_ENGINE) && (*req.iflags & FS_NEW_CHUNK_ENGINE);
      if (setNewChunkEngine && !config.iflags_chunk_engine()) {
        return makeError(MetaCode::kNoPermission, "FS_NEW_CHUNK_ENGINE disabled");
      }
      auto changed = *req.iflags ^ inode.acl.iflags;
      auto ownerChangeable = config.allow_owner_change_immutable() ? (uint32_t)(FS_HUGE_FILE_FL | FS_IMMUTABLE_FL)
                                                                   : (uint32_t)(FS_HUGE_FILE_FL);
      auto permCheck = req.user.isRoot() || (req.user.uid == inode.acl.uid && changed == (changed & ownerChangeable));
      if (!permCheck) {
        // NOTE: only allow root user set inode flags, file owner can use chattr +/- i, or set FS_HUGE_FILE_FL
        return makeError(MetaCode::kNoPermission, "only root can set iflags");
      }
    }
    if (req.perm.has_value() && *req.perm != inode.acl.perm && !req.user.isRoot() && req.user.uid != inode.acl.uid) {
      // man 2 chmod: The effective UID of the calling process must match the owner of the file, or the process must be
      // privileged (Linux: it must have the CAP_FOWNER capability).
      return makeError(MetaCode::kNoPermission, "no perm to set perm");
    }
    if (req.uid.has_value() && *req.uid != inode.acl.uid && !req.user.isRoot()) {
      // Only a privileged process (Linux: one with the CAP_CHOWN capability) may change the owner of a file.
      return makeError(MetaCode::kNoPermission, "no perm to set uid");
    }
    if (req.gid.has_value() && *req.gid != inode.acl.gid && !req.user.isRoot() &&
        (req.user.uid != inode.acl.uid || !req.user.inGroup(req.gid.value()))) {
      // The owner of a file may change the group of the file to any group of which that owner is a member.  A
      // privileged process (Linux: with CAP_CHOWN) may change the group arbitrarily.
      return makeError(MetaCode::kNoPermission, "no perm to set gid");
    }

    // permission check for utimes
    // To set both file timestamps to the current time (i.e., times is NULL, or both tv_nsec fields specify UTIME_NOW),
    // either:
    //   1. the caller must have write access to the file;
    //   2. the caller's effective user ID must match the owner of the file; or
    //   3. the caller must have appropriate privileges.
    // NOTE: we use UtcTime(0) as UTIME_NOW
    auto cond1 = inode.acl.checkPermission(req.user, AccessType::WRITE).hasValue();
    auto cond2 = req.user.uid == inode.acl.uid;
    auto cond3 = req.user.isRoot();
    if (req.atime || req.mtime) {
      if ((req.atime && req.atime != SETATTR_TIME_NOW) || (req.mtime && req.mtime != SETATTR_TIME_NOW)) {
        // To make any change other than setting both timestamps to the current time (i.e., times is not NULL, and
        // neither tv_nsec field is UTIME_NOW and neither tv_nsec field is UTIME_OMIT), either condition 2 or 3 above
        // must apply.
        if (!cond2 && !cond3) {
          return makeError(MetaCode::kNoPermission);
        }
      } else {
        if (!cond1 && !cond2 && !cond3) {
          return makeError(MetaCode::kNoPermission);
        }
      }
    }

    // permission check for setLayout
    if (req.layout) {
      if (!inode.isDirectory()) {
        return makeError(MetaCode::kNotDirectory, "setLayout but not directory");
      }
      RETURN_ON_ERROR(inode.acl.checkPermission(req.user, AccessType::WRITE));
    }

    if (!inode.isFile() && req.dynStripe) {
      return makeError(MetaCode::kNotFile, "extend dynStripe but not file");
    }

    return Void{};
  }

  static bool apply(Inode &inode, const SetAttrReq &req, Duration resolution, uint32_t stripeGrowth) {
    // now we can do update.
    bool dirty = false;

    // setPermission
    dirty |= update(inode.acl.iflags, req.iflags);
    dirty |= update(inode.acl.uid, req.uid);
    dirty |= update(inode.acl.gid, req.gid);
    dirty |= update(inode.acl.perm, req.perm);
    // setLayout
    if (req.layout.has_value()) {
      dirty |= update(inode.asDirectory().layout, req.layout);
    }
    if (dirty) {
      update(inode.ctime, SETATTR_TIME_NOW, resolution, true /* cmp */);
    }
    // utimes
    dirty |= update(inode.atime, req.atime, resolution, false /* cmp */);
    dirty |= update(inode.mtime, req.mtime, resolution, false /* cmp */);

    // extend
    if (req.dynStripe && inode.asFile().dynStripe && inode.asFile().dynStripe < req.dynStripe) {
      XLOGF_IF(FATAL, !inode.isFile(), "inode {} is not file", inode);

      auto growth = std::max(2u, stripeGrowth);
      auto dynStripe = inode.asFile().dynStripe;
      while (dynStripe < std::min(req.dynStripe, inode.asFile().layout.stripeSize)) {
        dynStripe = std::min(dynStripe * growth, inode.asFile().layout.stripeSize);
      }
      dirty |= update(inode.asFile().dynStripe, dynStripe);
    }

    return dirty;
  }

  static bool update(UtcTime &v, std::optional<UtcTime> nv, Duration resolution, bool cmp) {
    if (!nv) {
      return false;
    }
    if (*nv == SETATTR_TIME_NOW) {
      nv = UtcClock::now();
    }
    nv = nv->castGranularity(resolution);
    if (*nv != v && (!cmp || (*nv > v))) {
      v = *nv;
      return true;
    } else {
      return false;
    }
  }

  template <typename T>
  static bool update(T &v, std::optional<T> nv) {
    static_assert(!std::is_same_v<T, UtcTime>);
    if (nv.has_value() && nv != v) {
      v = *nv;
      return true;
    } else {
      return false;
    }
  }

  template <typename T>
  static bool update(T &v, T nv) {
    if (nv != v) {
      v = nv;
      return true;
    } else {
      return false;
    }
  }
};

}  // namespace hf3fs::meta::server