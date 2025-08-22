#include "UserStore.h"

#include <fmt/core.h>
#include <folly/logging/xlog.h>

#include "UserToken.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "core/user/UserCache.h"
#include "fdb/FDBTransaction.h"

namespace hf3fs::core {
namespace {
String packUid(Uid uid) {
  static constexpr auto prefix = kv::KeyPrefix::User;
  String buf;
  buf.reserve(sizeof(prefix) + sizeof(Uid::UnderlyingType));
  Serializer ser(buf);
  ser.put(prefix);
  ser.put(uid.toUnderType());
  return buf;
}

Result<Void> ensureTokenMatchUid(std::string_view token, flat::Uid uid) {
  auto newUid = decodeUidFromUserToken(token);
  RETURN_ON_ERROR(newUid);
  if (*newUid != uid) {
    return makeError(StatusCode::kInvalidFormat, "Could not set token from other uid");
  }
  return Void{};
}
}  // namespace

CoTryTask<std::optional<UserAttr>> UserStore::getUser(IReadOnlyTransaction &txn, Uid uid) {
  auto key = packUid(uid);
  auto getResult = co_await txn.snapshotGet(key);
  CO_RETURN_ON_ERROR(getResult);
  if (!getResult->has_value()) {
    co_return std::nullopt;
  }
  auto value = **getResult;
  XLOGF(DBG,
        "key value {:02x} {:02x}",
        fmt::join(key.data(), key.data(), ","),
        fmt::join(value.data(), value.data(), ","));
  UserAttr attr;
  CO_RETURN_ON_ERROR(serde::deserialize(attr, **getResult));
  attr.uid = uid;  // old versions of UserAttr didn't persist uid
  co_return attr;
}

CoTryTask<UserAttr> UserStore::addUser(IReadWriteTransaction &txn,
                                       Uid uid,
                                       String name,
                                       std::vector<Gid> groups,
                                       bool isRootUser,
                                       bool isAdmin,
                                       std::string_view token) {
  auto key = packUid(uid);
  auto getResult = co_await txn.snapshotGet(key);
  CO_RETURN_ON_ERROR(getResult);
  if (getResult->has_value()) co_return makeError(MetaCode::kExists);

  UserAttr attr;
  attr.name = std::move(name);
  attr.uid = uid;
  attr.gid = flat::Gid(uid);
  attr.groups = std::move(groups);
  attr.root = isRootUser;
  attr.admin = isAdmin;

  if (!token.empty()) {
    CO_RETURN_ON_ERROR(ensureTokenMatchUid(token, uid));
    attr.token = token;
  } else {
    auto tokenResult = co_await encodeUserToken(uid, txn);
    CO_RETURN_ON_ERROR(tokenResult);
    attr.token = *tokenResult;
  }

  CO_RETURN_ON_ERROR(co_await txn.set(key, serde::serialize(attr)));
  co_return attr;
}

CoTryTask<void> UserStore::removeUser(IReadWriteTransaction &txn, Uid uid) {
  auto fetchResult = co_await getUser(txn, uid);
  CO_RETURN_ON_ERROR(fetchResult);
  if (!fetchResult->has_value()) {
    co_return makeError(MetaCode::kNotFound, fmt::format("Uid {} not found", uid.toUnderType()));
  }
  auto key = packUid(uid);
  co_return co_await txn.clear(key);
}

CoTryTask<UserAttr> UserStore::setUserAttr(IReadWriteTransaction &txn,
                                           Uid uid,
                                           std::string_view newName,
                                           const std::vector<Gid> *gids,
                                           std::optional<bool> isRootUser,
                                           std::optional<bool> isAdmin) {
  auto fetchResult = co_await getUser(txn, uid);
  CO_RETURN_ON_ERROR(fetchResult);
  if (!fetchResult->has_value()) {
    co_return makeError(MetaCode::kNotFound, fmt::format("Uid {} not found", uid.toUnderType()));
  }

  const auto &user = **fetchResult;
  auto newUser = user;

  if (!newName.empty()) {
    newUser.name = newName;
  }

  if (gids) {
    newUser.groups = *gids;
  }
  if (isRootUser) {
    newUser.root = *isRootUser;
  }
  if (isAdmin) {
    newUser.admin = *isAdmin;
  }
  if (newUser != user) {
    CO_RETURN_ON_ERROR(co_await txn.set(packUid(uid), serde::serialize(newUser)));
  }
  co_return newUser;
}

CoTryTask<UserAttr> UserStore::setUserToken(IReadWriteTransaction &txn,
                                            Uid uid,
                                            std::string_view newToken,
                                            bool invalidateExistedToken,
                                            size_t maxTokens,
                                            Duration tokenLifetimeExtendLength) {
  auto fetchResult = co_await getUser(txn, uid);
  CO_RETURN_ON_ERROR(fetchResult);
  if (!fetchResult->has_value()) {
    co_return makeError(MetaCode::kNotFound, fmt::format("Uid {} not found", uid.toUnderType()));
  }

  const auto &user = **fetchResult;
  auto newUser = user;

  String newTokenStr(newToken);
  if (!newTokenStr.empty()) {
    CO_RETURN_ON_ERROR(ensureTokenMatchUid(newTokenStr, uid));
  } else {
    auto tokenResult = co_await encodeUserToken(uid, txn);
    CO_RETURN_ON_ERROR(tokenResult);
    newTokenStr = std::move(*tokenResult);
  }
  CO_RETURN_ON_ERROR(
      newUser.addNewToken(newTokenStr, UtcClock::now(), invalidateExistedToken, maxTokens, tokenLifetimeExtendLength));
  if (newUser != user) {
    CO_RETURN_ON_ERROR(co_await txn.set(packUid(uid), serde::serialize(newUser)));
  }
  co_return newUser;
}

CoTryTask<std::vector<UserAttr>> UserStore::listUsers(IReadOnlyTransaction &txn) {
  auto listRes = co_await kv::TransactionHelper::listByPrefix(txn, kv::toStr(kv::KeyPrefix::User), {});
  CO_RETURN_ON_ERROR(listRes);

  std::vector<UserAttr> res;
  for (const auto &kv : *listRes) {
    kv::KeyPrefix keyPrefix;
    Uid::UnderlyingType uid;
    CO_RETURN_ON_ERROR(Deserializer::deserRawArgs(kv.key, keyPrefix, uid));

    UserAttr attr;
    CO_RETURN_ON_ERROR(serde::deserialize(attr, kv.value));
    attr.uid = Uid(uid);
    res.push_back(std::move(attr));
  }
  co_return res;
}

}  // namespace hf3fs::core
