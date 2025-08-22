#pragma once

#include <algorithm>
#include <cstdint>
#include <folly/Utility.h>
#include <folly/dynamic.h>
#include <folly/hash/Checksum.h>
#include <folly/json.h>
#include <folly/logging/Logger.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "common/serde/Serde.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Path.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::meta::server {

struct Event {
  enum class Type { Create, Mkdir, HardLink, Remove, Truncate, OpenWrite, CloseWrite, Rename, Symlink, GC };

  Type type;
  folly::dynamic data;

  Event(Type type)
      : Event(type, folly::dynamic::object()) {
    addField("event", magic_enum::enum_name(type));
    addField("ts", UtcClock::now().toMicroseconds());
  }
  Event(Type type, folly::dynamic data)
      : type(type),
        data(std::move(data)) {}

  void log() const;
  void log(const folly::json::serialization_opts &opts) const;

  Event &addField(folly::dynamic key, folly::dynamic value) {
    data.insert(std::move(key), std::move(value));
    return *this;
  }
};

struct MetaEventTrace {
  SERDE_STRUCT_FIELD(eventType, Event::Type::Create);
  SERDE_STRUCT_FIELD(inodeId, InodeId());
  SERDE_STRUCT_FIELD(parentId, InodeId());
  SERDE_STRUCT_FIELD(entryName, std::string());
  SERDE_STRUCT_FIELD(dstParentId, InodeId());
  SERDE_STRUCT_FIELD(dstEntryName, std::string());
  SERDE_STRUCT_FIELD(ownerId, Uid(0));
  SERDE_STRUCT_FIELD(userId, Uid());
  SERDE_STRUCT_FIELD(client, ClientId{Uuid::zero()});
  SERDE_STRUCT_FIELD(tableId, flat::ChainTableId());
  SERDE_STRUCT_FIELD(inodeType, InodeType::File);
  SERDE_STRUCT_FIELD(nlink, uint16_t(0));
  SERDE_STRUCT_FIELD(length, uint64_t(0));
  SERDE_STRUCT_FIELD(truncateVer, uint64_t(0));
  SERDE_STRUCT_FIELD(dynStripe, uint32_t(0));
  SERDE_STRUCT_FIELD(oflags, OpenFlags());
  SERDE_STRUCT_FIELD(recursiveRemove, false);
  SERDE_STRUCT_FIELD(removedChunks, size_t(0));
  SERDE_STRUCT_FIELD(pruneSession, false);
  SERDE_STRUCT_FIELD(symLinkTarget, Path());
  SERDE_STRUCT_FIELD(origPath, Path());
};

}  // namespace hf3fs::meta::server
