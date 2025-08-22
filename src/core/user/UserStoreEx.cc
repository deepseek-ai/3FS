#include "UserStoreEx.h"

#include "UserToken.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/WithTransaction.h"
#include "fdb/FDBRetryStrategy.h"

namespace hf3fs::core {
UserStoreEx::UserStoreEx(kv::IKVEngine &kvEngine,
                         const kv::TransactionRetry &transactionRetry,
                         const UserCache::Config &cacheCfg)
    : kvEngine_(kvEngine),
      transactionRetry_(transactionRetry),
      userCache_(cacheCfg) {}

CoTryTask<flat::UserAttr> UserStoreEx::getUser(std::string_view token) {
  auto uid = decodeUidFromUserToken(token);
  if (UNLIKELY(uid.hasError())) {
    co_return makeError(StatusCode::kAuthenticationFail, "Token decode failed");
  }
  auto user = co_await getUser(*uid);
  CO_RETURN_ON_ERROR(user);
  if (auto res = user->validateToken(token, UtcClock::now()); res.hasError()) {
    XLOGF(ERR, "Token validate failed: {}, uid {}", res.error(), *uid);
    co_return makeError(StatusCode::kAuthenticationFail, "Token validate failed");
  }
  co_return *user;
}

CoTryTask<void> UserStoreEx::authenticate(flat::UserInfo &userInfo) {
  std::vector<Gid> gids;
  auto userFromToken = co_await getUser(userInfo.token);
  CO_RETURN_ON_ERROR(userFromToken);

  if (userFromToken->root) {
    XLOGF(DBG, "Root user {}, uid {}, euid {}", userFromToken->name, userFromToken->uid, userInfo.uid);
    if (userInfo.uid != userFromToken->uid) {
      // root users can act as anyone
      userFromToken = co_await getUser(userInfo.uid);
      CO_RETURN_ON_ERROR(userFromToken);
    }
  } else {
    if (userInfo.uid != userFromToken->uid || userInfo.gid != userFromToken->gid) {
      auto msg = fmt::format("User not match, userInfo {}, (uid {}, gid {})",
                             userInfo,
                             userFromToken->uid.toUnderType(),
                             userFromToken->gid.toUnderType());
      XLOGF(ERR, "{}", msg);
      co_return makeError(StatusCode::kAuthenticationFail, std::move(msg));
    }
  }

  userInfo.groups = std::move(userFromToken->groups);
  co_return Void{};
}

CoTryTask<flat::UserAttr> UserStoreEx::getUser(flat::Uid uid) {
  auto getResult = userCache_.get(uid);
  if (getResult.attr) {
    co_return *getResult.attr;
  }
  if (getResult.marked) {
    co_return makeError(StatusCode::kAuthenticationFail, fmt::format("Uid {} not found", uid.toUnderType()));
  }
  auto strategy = kv::FDBRetryStrategy(
      {transactionRetry_.max_backoff(), transactionRetry_.max_retry_count(), /*retryMaybeCommitted=*/true});
  auto result = co_await kv::WithTransaction(strategy).run(
      kvEngine_.createReadonlyTransaction(),
      [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::optional<UserAttr>> {
        co_return co_await userStore_.getUser(txn, uid);
      });
  if (result.hasError()) {
    CO_RETURN_ERROR(result);
  } else {
    userCache_.set(uid, *result);
    if (result->has_value()) {
      co_return **result;
    } else {
      co_return makeError(StatusCode::kAuthenticationFail, fmt::format("Uid {} not found", uid.toUnderType()));
    }
  }
}
}  // namespace hf3fs::core
