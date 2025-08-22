#include "meta/store/FileSession.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "fbs/meta/Common.h"

// todo: move ito meta/utils
#define FMT_KEY(key) fmt::join((const uint8_t *)key.data(), (const uint8_t *)key.data() + key.size(), ",")

namespace hf3fs::meta::server {

namespace {

struct SessionByInode {
  static constexpr auto keyPrefix = kv::KeyPrefix::InodeSession;

  static std::string prefixOf(const InodeId inodeId) { return Serializer::serRawArgs(keyPrefix, inodeId.packKey()); }
  static std::string packKey(InodeId inodeId, Uuid sessionId) {
    return Serializer::serRawArgs(keyPrefix, inodeId.packKey(), sessionId);
  }
  static std::pair<InodeId, Uuid> unpackKey(std::string_view key) {
    kv::KeyPrefix prefix;
    InodeId::Key inodeId;
    Uuid sessionId;
    auto result = Deserializer::deserRawArgs(key, prefix, inodeId, sessionId);
    XLOGF_IF(DFATAL, result.hasError(), "Failed to unpack key {:02x}, err {}", FMT_KEY(key), result.error());
    XLOGF_IF(DFATAL,
             prefix != keyPrefix,
             "SessionByInode prefix not match {} != {}",
             (uint32_t)prefix,
             (uint32_t)keyPrefix);
    return {InodeId::unpackKey(inodeId), sessionId};
  }

  static Result<FileSession> unpack(std::string_view key, std::string_view value) {
    auto [inodeId, sessionId] = unpackKey(key);
    FileSession session;
    RETURN_ON_ERROR(serde::deserialize(session, value));
    if (session.inodeId != inodeId || session.sessionId != sessionId) {
      XLOGF(DFATAL,
            "SessionByInode KV not match, key {} -> {} {}, value {}",
            FMT_KEY(key),
            inodeId,
            sessionId,
            session);
      return makeError(StatusCode::kDataCorruption);
    }
    return session;
  }
};

// struct SessionByClient {
//   static constexpr auto keyPrefix = kv::KeyPrefix::ClientSession;
//   static std::string prefixOf(const Uuid &clientId) { return Serializer::serRawArgs(keyPrefix, clientId); }
//   static std::string packKey(const Uuid &clientId, const Uuid &sessionId) {
//     return Serializer::serRawArgs(keyPrefix, clientId, sessionId);
//   }
//   static std::pair<Uuid, Uuid> unpackKey(std::string_view key) {
//     kv::KeyPrefix prefix;
//     Uuid clientId;
//     Uuid sessionId;
//     auto result = Deserializer::deserRawArgs(key, prefix, clientId, sessionId);
//     XLOGF_IF(DFATAL, result.hasError(), "Failed to unpack key {:02x}, err {}", FMT_KEY(key), result.error());
//     XLOGF_IF(DFATAL,
//              prefix != keyPrefix,
//              "SessionByInode prefix not match {} != {}",
//              (uint32_t)prefix,
//              (uint32_t)keyPrefix);
//     return {clientId, sessionId};
//   }
//   static Result<FileSession> unpack(std::string_view key, std::string_view value) {
//     auto [clientId, sessionId] = unpackKey(key);
//     FileSession session;
//     RETURN_ON_ERROR(serde::deserialize(session, value));
//     if (session.clientId.uuid != clientId || session.sessionId != sessionId) {
//       XLOGF(DFATAL,
//             "SessionByClient KV not match, key {} -> {} {}, value {}",
//             FMT_KEY(key),
//             clientId,
//             sessionId,
//             session);
//       return makeError(StatusCode::kDataCorruption);
//     }
//     return session;
//   }
// };

template <typename SessionType, typename Id>
CoTryTask<std::vector<FileSession>> listSessions(IReadOnlyTransaction &txn, Id id, bool snapshot, size_t limit) {
  auto prefix = SessionType::prefixOf(id);
  auto unpack = SessionType::unpack;
  auto options = kv::TransactionHelper::ListByPrefixOptions().withSnapshot(snapshot).withLimit(limit);
  co_return co_await kv::TransactionHelper::listByPrefix<FileSession>(txn, prefix, options, unpack);
}

template <typename SessionType, typename Id>
CoTryTask<std::optional<FileSession>> loadSession(IReadOnlyTransaction &txn, Id id, Uuid sessionId) {
  XLOGF(DBG, "Load session {}, {}", id, sessionId);
  auto key = SessionType::packKey(id, sessionId);
  auto value = co_await txn.get(key);
  CO_RETURN_ON_ERROR(value);
  if (value->has_value()) {
    auto result = SessionType::unpack(key, **value);
    CO_RETURN_ON_ERROR(result);
    XLOGF(DBG, "Load session found {}", *result);
    co_return result;
  }
  co_return std::nullopt;
}

}  // namespace

/** FileSession */
std::string FileSession::prefix(InodeId inodeId) { return SessionByInode::prefixOf(inodeId); }

std::string FileSession::packKey(InodeId inodeId, Uuid sessionId) {
  return SessionByInode::packKey(inodeId, sessionId);
}

Result<std::pair<InodeId, Uuid>> FileSession::unpackByInodeKey(std::string_view key) {
  return SessionByInode::unpackKey(key);
}

Result<FileSession> FileSession::unpack(std::string_view key, std::string_view value) {
  FileSession session;
  auto result = serde::deserialize(session, value);
  if (result.hasError()) {
    XLOGF(DFATAL,
          "FileSession unpack failed, key {}, value {}, error {}",
          FMT_KEY(key),
          FMT_KEY(value),
          result.error());
    RETURN_ERROR(result);
  }
  session.payload = "";
  return session;
}

CoTryTask<std::optional<FileSession>> FileSession::load(IReadOnlyTransaction &txn, InodeId inodeId, Uuid session) {
  co_return co_await loadSession<SessionByInode, InodeId>(txn, inodeId, session);
}

CoTryTask<std::vector<FileSession>> FileSession::list(IReadOnlyTransaction &txn,
                                                      InodeId inodeId,
                                                      bool snapshot,
                                                      size_t limit) {
  co_return co_await listSessions<SessionByInode>(txn, inodeId, snapshot, limit);
}

CoTryTask<std::optional<FileSession>> FileSession::checkExists(IReadWriteTransaction &txn, const InodeId inodeId) {
  auto exists = co_await snapshotCheckExists(dynamic_cast<IReadOnlyTransaction &>(txn), inodeId);
  CO_RETURN_ON_ERROR(exists);

  if (!exists) {
    // NOTE: add range into read conflict set
    auto prefix = SessionByInode::prefixOf(inodeId);
    auto end = kv::TransactionHelper::prefixListEndKey(prefix);
    CO_RETURN_ON_ERROR(co_await txn.addReadConflictRange(kv::TransactionHelper::keyAfter(prefix), end));
  }

  co_return exists;
}

CoTryTask<std::optional<FileSession>> FileSession::snapshotCheckExists(IReadOnlyTransaction &txn,
                                                                       const InodeId inodeId) {
  auto prefix = SessionByInode::prefixOf(inodeId);
  auto end = kv::TransactionHelper::prefixListEndKey(prefix);
  auto result = co_await txn.snapshotGetRange({prefix, false}, {end, false}, 1);
  CO_RETURN_ON_ERROR(result);

  XLOGF(DBG, "Check session for inodeId {}, cnt {} hasMore {}", inodeId, result->kvs.size(), result->hasMore);

  while (result->kvs.empty() && result->hasMore) {
    result = co_await txn.snapshotGetRange({prefix, false}, {end, false}, 1);
    CO_RETURN_ON_ERROR(result);
    XLOGF(DBG, "Check session for inodeId {}, cnt {} hasMore {}", inodeId, result->kvs.size(), result->hasMore);
  }

  if (!result->kvs.empty()) {
    auto &kv = result->kvs.at(0);
    co_return SessionByInode::unpack(kv.key, kv.value);
  }

  co_return std::nullopt;
}

CoTryTask<void> FileSession::removeAll(IReadWriteTransaction &txn, InodeId inodeId) {
  /* todo: may be can't remove all sessions in 1 transactions */
  XLOGF(DBG, "SessionManager remove all sessions for {}", inodeId);
  auto sessions = co_await list(txn, inodeId, false);
  CO_RETURN_ON_ERROR(sessions);
  for (const auto &session : *sessions) {
    CO_RETURN_ON_ERROR(co_await session.remove(txn));
  }
  XLOGF_IF(DBG, !sessions->empty(), "SessionManager remove {} sessions of inodeId {}.", sessions->size(), inodeId);
  co_return Void{};
}

CoTryTask<void> FileSession::store(IReadWriteTransaction &txn) const {
  XLOGF(DBG, "Store session {}", *this);

  // TODO: what if two client generate the same sessionId? should we check its existence at first?
  auto value = serde::serialize(*this);
  auto keyByInode = SessionByInode::packKey(inodeId, sessionId);
  CO_RETURN_ON_ERROR(co_await txn.set(keyByInode, value));
  // auto keyByClient = SessionByClient::packKey(clientId.uuid, sessionId);
  // CO_RETURN_ON_ERROR(co_await txn.set(keyByClient, value));
  co_return Void{};
}

CoTryTask<void> FileSession::remove(IReadWriteTransaction &txn) const {
  XLOGF(DBG, "Remove session {}", *this);

  auto keyByInode = SessionByInode::packKey(inodeId, sessionId);
  CO_RETURN_ON_ERROR(co_await txn.clear(keyByInode));
  // auto keyByClient = SessionByClient::packKey(clientId.uuid, sessionId);
  // CO_RETURN_ON_ERROR(co_await txn.clear(keyByClient));

  co_return Void{};
}

CoTryTask<std::vector<FileSession>> FileSession::scan(IReadOnlyTransaction &txn,
                                                      size_t shard,
                                                      std::optional<FileSession> prev) {
  if (shard >= kShard) {
    co_return std::vector<FileSession>();
  }

  auto beginKey = SessionByInode::packKey(InodeId(shard), Uuid::max());
  if (prev) {
    auto prevKey = SessionByInode::packKey(prev->inodeId, prev->sessionId);
    beginKey = std::max(beginKey, prevKey);
  }
  auto endKey = SessionByInode::packKey(InodeId(shard + 1), Uuid::zero());
  if (shard + 1 >= kShard) {
    endKey = SessionByInode::packKey(InodeId(~0ULL), Uuid::zero());
  }
  if (beginKey >= endKey) {
    co_return std::vector<FileSession>();
  }

  auto result = co_await txn.getRange({beginKey, false}, {endKey, false}, 512);
  CO_RETURN_ON_ERROR(result);

  std::vector<FileSession> sessions;
  for (auto &[key, value] : result->kvs) {
    auto session = FileSession::unpack(key, value);
    CO_RETURN_ON_ERROR(session);
    sessions.push_back(*session);
  }
  co_return sessions;
}

// std::string FileSession::prefix(Uuid clientId) { return SessionByClient::prefixOf(clientId); }
//
// std::string FileSession::packKey(Uuid clientId, Uuid sessionId) {
//   return SessionByClient::packKey(clientId, sessionId);
// }
//
// Result<std::pair<Uuid, Uuid>> FileSession::unpackByClientKey(std::string_view key) {
//   return SessionByClient::unpackKey(key);
// }
//
// CoTryTask<std::optional<FileSession>> FileSession::load(IReadOnlyTransaction &txn, ClientId clientId, Uuid session) {
//   co_return co_await loadSession<SessionByClient, Uuid>(txn, clientId.uuid, session);
// }
//
// CoTryTask<std::vector<FileSession>> FileSession::list(IReadOnlyTransaction &txn, Uuid clientId, bool snapshot) {
//   co_return co_await listSessions<SessionByClient>(txn, clientId, snapshot);
// }

}  // namespace hf3fs::meta::server