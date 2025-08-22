#pragma once

#include "UserCache.h"
#include "UserStore.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/TransactionRetry.h"

namespace hf3fs::core {

class UserStoreEx {
 public:
  UserStoreEx(kv::IKVEngine &kvEngine, const kv::TransactionRetry &transactionRetry, const UserCache::Config &cacheCfg);

  CoTryTask<void> authenticate(flat::UserInfo &userInfo);
  CoTryTask<flat::UserAttr> getUser(std::string_view token);
  CoTryTask<flat::UserAttr> getUser(flat::Uid uid);

  UserCache &cache() { return userCache_; }

 private:
  kv::IKVEngine &kvEngine_;
  const kv::TransactionRetry &transactionRetry_;
  UserStore userStore_;
  UserCache userCache_;
};
}  // namespace hf3fs::core
