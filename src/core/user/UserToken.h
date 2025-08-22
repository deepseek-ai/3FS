#pragma once

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/core/user/User.h"

namespace hf3fs::core {
String encodeUserToken(uint32_t uid, uint64_t timestamp);
CoTryTask<String> encodeUserToken(uint32_t uid, kv::IReadOnlyTransaction &txn);

Result<std::pair<uint32_t, uint64_t>> decodeUserToken(std::string_view token);
Result<flat::Uid> decodeUidFromUserToken(std::string_view token);
}  // namespace hf3fs::core
