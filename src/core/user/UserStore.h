#pragma once

#include "common/kv/ITransaction.h"
#include "core/user/UserCache.h"
#include "fbs/core/user/User.h"

namespace hf3fs::core {
using flat::Gid;
using flat::Uid;
using flat::UserAttr;
using flat::UserInfo;
using hf3fs::kv::IReadOnlyTransaction;
using hf3fs::kv::IReadWriteTransaction;

class UserStore {
 public:
  UserStore() = default;

  CoTryTask<std::optional<UserAttr>> getUser(IReadOnlyTransaction &txn, Uid uid);

  CoTryTask<UserAttr> addUser(IReadWriteTransaction &txn,
                              Uid uid,
                              String name,
                              std::vector<Gid> groups,
                              bool isRootUser,
                              bool isAdmin = false,
                              std::string_view token = {});

  CoTryTask<void> removeUser(IReadWriteTransaction &txn, Uid uid);

  CoTryTask<UserAttr> setUserAttr(IReadWriteTransaction &txn,
                                  Uid uid,
                                  std::string_view newName,
                                  const std::vector<Gid> *gids,
                                  std::optional<bool> isRootUser,
                                  std::optional<bool> isAdmin);

  CoTryTask<UserAttr> setUserToken(IReadWriteTransaction &txn,
                                   Uid uid,
                                   std::string_view newToken,
                                   bool invalidateExistedToken,
                                   size_t maxTokens,
                                   Duration tokenLifetimeExtendLength);

  CoTryTask<std::vector<UserAttr>> listUsers(IReadOnlyTransaction &txn);
};
}  // namespace hf3fs::core
