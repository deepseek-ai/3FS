#pragma once

#include "common/app/NodeId.h"
#include "common/utils/Address.h"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"
#include "common/utils/Uuid.h"

namespace hf3fs::flat {
enum class PublicTargetState : uint8_t {
  INVALID = 0,
  SERVING = 1,
  LASTSRV = 2,
  SYNCING = 4,
  WAITING = 8,
  OFFLINE = 16,
  MIN = INVALID,
  MAX = OFFLINE
};

enum class LocalTargetState : uint8_t {
  INVALID = 0,
  UPTODATE = 1,
  ONLINE = 2,
  OFFLINE = 4,
  MIN = INVALID,
  MAX = OFFLINE
};

enum class NodeStatus : uint8_t {
  PRIMARY_MGMTD = 0,
  HEARTBEAT_CONNECTED = 1,
  HEARTBEAT_CONNECTING = 2,
  HEARTBEAT_FAILED = 3,
  DISABLED = 4,
  MIN = HEARTBEAT_CONNECTED,
  MAX = DISABLED
};

enum class NodeType : uint8_t { MGMTD = 0, META = 1, STORAGE = 2, CLIENT = 3, FUSE = 4, MIN = MGMTD, MAX = FUSE };

enum class SetTagMode : uint8_t { REPLACE = 0, UPSERT = 1, REMOVE = 2, MIN = REPLACE, MAX = REMOVE };

STRONG_TYPEDEF(uint64_t, TargetId);
STRONG_TYPEDEF(uint64_t, HeartbeatVersion);
STRONG_TYPEDEF(uint64_t, RoutingInfoVersion);
STRONG_TYPEDEF(uint64_t, ConfigVersion);
STRONG_TYPEDEF(uint64_t, ClientSessionVersion);

// ChainVersion is space sensitive and uint32 is enough for changes of one chain
STRONG_TYPEDEF(uint32_t, ChainVersion);

STRONG_TYPEDEF(uint32_t, ChainTableId);
STRONG_TYPEDEF(uint32_t, ChainTableVersion);
STRONG_TYPEDEF(uint32_t, ChainId);

}  // namespace hf3fs::flat
