#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/Utility.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/String.h"

namespace hf3fs {

/**
 * Generate 64bits unique id range from 1 to uint64_t::max.
 *
 * The uniqueness of generated ids is guaranteed by transaction. IdAllocator can use multiple keys in FoundationDB to
 * avoid transaction conflict, strictly monotonically increasing isn't guaranteed when numShard > 1.
 */
template <typename RetryStrategy>
class IdAllocator : folly::MoveOnly {
 public:
  IdAllocator(kv::IKVEngine &kvEngine, RetryStrategy strategy, String keyPrefix, uint8_t numShard)
      : kvEngine_(kvEngine),
        strategy_(strategy),
        keyPrefix_(std::move(keyPrefix)),
        numShard_(numShard),
        shardIdx_(0),
        shardList_() {
    XLOGF_IF(FATAL, numShard_ == 0, "Shared shouldn't be zero!!!");
    shardList_.reserve(numShard_);
    for (uint8_t i = 0; i < numShard_; i++) {
      shardList_.push_back(i);
    }
    std::shuffle(shardList_.begin(), shardList_.end(), std::mt19937_64(folly::Random::rand64()));
  }

  CoTryTask<uint64_t> allocate() {
    auto txn = kvEngine_.createReadWriteTransaction();
    auto result = co_await kv::WithTransaction<RetryStrategy>(strategy_).run(
        *txn,
        [&](kv::IReadWriteTransaction &txn) -> CoTryTask<uint64_t> { co_return co_await allocateTxn(txn); });

    XLOGF_IF(ERR, result.hasError(), "Failed to allocate ID, error {}", result.error());
    co_return result;
  }

  CoTryTask<std::vector<uint64_t>> status() {
    auto txn = kvEngine_.createReadonlyTransaction();
    auto result = co_await kv::WithTransaction<RetryStrategy>(strategy_).run(
        *txn,
        [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::vector<uint64_t>> { co_return co_await statusTxn(txn); });

    XLOGF_IF(ERR, result.hasError(), "Failed to query status, error {}", result.error());
    co_return result;
  }

 private:
  std::string getShardKey(uint8_t shard) {
    XLOGF_IF(FATAL, shard > numShard_, "{} > {}", shard, numShard_);
    return fmt::format("{}-{}", keyPrefix_, shard);
  }

  Result<uint64_t> unpackShardValue(uint8_t shard, std::optional<String> value) {
    uint64_t minVal = shard == 0 ? 1 : 0;
    uint64_t val = minVal;
    if (value.has_value()) {
      if (UNLIKELY(value->size() != sizeof(uint64_t))) {
        XLOGF(CRITICAL,
              "Value of shard {} key {} has a unexpected length {} != 8.",
              shard,
              getShardKey(shard),
              value->size());
        return makeError(StatusCode::kDataCorruption);
      }
      val = folly::Endian::little64(folly::loadUnaligned<uint64_t>(value->data()));
      if (UNLIKELY(val < minVal)) {
        XLOGF(CRITICAL, "Value of shard {} key {} has a val {} < minVal {}.", shard, getShardKey(shard), val, minVal);
        return makeError(StatusCode::kDataCorruption);
      }
    }

    return val;
  }

  CoTryTask<uint64_t> allocateTxn(kv::IReadWriteTransaction &txn) {
    auto shardIdx = shardIdx_++;
    auto shard = shardList_[shardIdx % shardList_.size()];
    auto shardKey = getShardKey(shard);

    // load old value
    auto getResult = co_await txn.get(shardKey);
    CO_RETURN_ON_ERROR(getResult);
    auto unpackResult = unpackShardValue(shard, getResult.value());
    CO_RETURN_ON_ERROR(unpackResult);
    uint64_t val = unpackResult.value();

    // set new value
    auto bytes = folly::bit_cast<std::array<char, 8>>(folly::Endian::little64(val + 1));
    auto setResult = co_await txn.set(shardKey, std::string_view(bytes.begin(), bytes.size()));
    CO_RETURN_ON_ERROR(setResult);

    XLOGF(DBG, "IdAllocator allocate from shard {}, val {}, id {}\n", shard, val, val * numShard_ + shard);

    co_return (val * numShard_) + shard;
  }

  CoTryTask<std::vector<uint64_t>> statusTxn(kv::IReadOnlyTransaction &txn) {
    std::vector<uint64_t> vec;
    for (uint8_t shard = 0; shard < numShard_; shard++) {
      auto shardKey = getShardKey(shard);
      auto getResult = co_await txn.snapshotGet(shardKey);
      CO_RETURN_ON_ERROR(getResult);
      auto unpackResult = unpackShardValue(shard, getResult.value());
      CO_RETURN_ON_ERROR(unpackResult);
      vec.push_back(unpackResult.value());
    }

    co_return vec;
  }

  kv::IKVEngine &kvEngine_;
  RetryStrategy strategy_;
  String keyPrefix_;
  uint8_t numShard_;
  std::atomic<size_t> shardIdx_;
  std::vector<uint8_t> shardList_;
};

}  // namespace hf3fs
