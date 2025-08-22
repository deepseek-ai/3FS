#pragma once

#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <folly/Overload.h>
#include <folly/logging/xlog.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

template <typename T>
inline InodeId getInodeId(T &&val) {
  return folly::variant_match(
      std::forward<T>(val),
      [](const Inode &inode) { return inode.id; },
      [](const DirEntry &entry) { return entry.id; },
      [](const InodeId &id) { return id; },
      [](const std::pair<InodeId, Acl> &cachedAcl) { return cachedAcl.first; });
}

template <typename T>
inline Acl getDirectoryAcl(T &&val) {
  return folly::variant_match(
      std::forward<T>(val),
      [](const Inode &inode) {
        assert(inode.isDirectory());
        return inode.acl;
      },
      [](const DirEntry &entry) {
        assert(entry.isDirectory() && entry.dirAcl.has_value());
        return *entry.dirAcl;
      },
      [](const std::pair<InodeId, Acl> &cachedAcl) { return cachedAcl.second; });
}

template <typename T>
inline InodeType getInodeType(T &&val) {
  return folly::variant_match(
      std::forward<T>(val),
      [](const Inode &inode) { return inode.getType(); },
      [](const DirEntry &entry) { return entry.type; },
      [](const std::pair<InodeId, Acl> &) { return InodeType::Directory; });
}

template <typename T>
inline Result<T> checkMetaFound(std::optional<T> val) {
  if (!val.has_value()) {
    return makeError(MetaCode::kNotFound);
  }
  return std::move(val.value());
}

inline bool isFirstMeta(client::ICommonMgmtdClient &mgmtd, flat::NodeId nodeId) {
  auto routing = mgmtd.getRoutingInfo();
  if (!routing) {
    return false;
  }
  auto nodes = routing->getNodeBy(flat::selectNodeByType(flat::NodeType::META) && flat::selectActiveNode());
  auto first =
      std::min_element(nodes.begin(), nodes.end(), [](auto &a, auto &b) { return a.app.nodeId < b.app.nodeId; });
  return first != nodes.end() && first->app.nodeId == nodeId;
}

}  // namespace hf3fs::meta::server
