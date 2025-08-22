#pragma once

#include <folly/logging/xlog.h>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/app/ClientId.h"
#include "common/kv/ITransaction.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::meta::server {

using kv::IReadOnlyTransaction;
using kv::IReadWriteTransaction;

struct FileSession {
  SERDE_STRUCT_FIELD(inodeId, InodeId());
  SERDE_STRUCT_FIELD(clientId, ClientId::zero());
  SERDE_STRUCT_FIELD(sessionId, Uuid::zero());
  SERDE_STRUCT_FIELD(timestamp, UtcTime());
  SERDE_STRUCT_FIELD(payload, std::string());  // for placeholder

 public:
  static std::string prefix(InodeId inodeId);
  static std::string packKey(InodeId inodeId, Uuid session);
  static Result<std::pair<InodeId, Uuid>> unpackByInodeKey(std::string_view key);

  static FileSession create(InodeId inodeId, SessionInfo session, UtcTime timestamp = UtcClock::now()) {
    return {inodeId, session.client, session.session, timestamp};
  }
  static FileSession create(InodeId inodeId, ClientId clientId, Uuid sessionId, UtcTime timestamp = UtcClock::now()) {
    return {inodeId, clientId, sessionId, timestamp};
  }

  static Result<FileSession> unpack(std::string_view key, std::string_view value);

  static CoTryTask<std::optional<FileSession>> load(IReadOnlyTransaction &txn, InodeId inodeId, Uuid session);
  static CoTryTask<std::vector<FileSession>> list(IReadOnlyTransaction &txn,
                                                  InodeId inodeId,
                                                  bool snapshot,
                                                  size_t limit = 0);

  static CoTryTask<std::optional<FileSession>> snapshotCheckExists(IReadOnlyTransaction &txn, const InodeId inodeId);
  static CoTryTask<std::optional<FileSession>> checkExists(IReadWriteTransaction &txn, const InodeId inodeId);
  static CoTryTask<void> removeAll(IReadWriteTransaction &txn, InodeId inodeId);

  static constexpr size_t kShard = 256;
  static_assert(kShard == (1 << 8));
  static CoTryTask<std::vector<FileSession>> scan(IReadOnlyTransaction &txn,
                                                  size_t shard,
                                                  std::optional<FileSession> prev);

  CoTryTask<void> store(IReadWriteTransaction &txn) const;
  CoTryTask<void> remove(IReadWriteTransaction &txn) const;

  // prune, store FileSessions need to be pruned under special InodeId(-1)
  static FileSession createPrune(ClientId clientId, Uuid sessionId) {
    return FileSession::create(InodeId(-1), clientId, sessionId);
  }

  static CoTryTask<std::vector<FileSession>> listPrune(IReadOnlyTransaction &txn, size_t limit) {
    co_return co_await FileSession::list(txn, InodeId(-1), true, limit);
  }

  // static CoTryTask<std::optional<FileSession>> load(IReadOnlyTransaction &txn, ClientId clientId, Uuid session);
  // static CoTryTask<std::vector<FileSession>> list(IReadOnlyTransaction &txn, Uuid clientId, bool snapshot);
  // static std::string prefix(Uuid clientId);
  // static std::string packKey(Uuid clientId, Uuid session);
  // static Result<std::pair<Uuid, Uuid>> unpackByClientKey(std::string_view key);
};

}  // namespace hf3fs::meta::server