#include "storage/store/ChunkMetaStore.h"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <limits>

#include "common/monitor/Recorder.h"
#include "common/serde/BigEndian.h"
#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/Size.h"
#include "common/utils/UtcTime.h"
#include "fbs/storage/Common.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder storageMetaGet{"storage.meta_get"};
monitor::OperationRecorder storageMetaSet{"storage.meta_set"};
monitor::OperationRecorder storageMetaRemove{"storage.meta_remove"};
monitor::OperationRecorder storageMetaCreate{"storage.meta_create"};
monitor::OperationRecorder storageMetaRecycle{"storage.meta_recycle"};
monitor::LatencyRecorder storageMetaCreateWait{"storage.meta_create.wait_latency"};
monitor::LatencyRecorder storageMetaCreateCommit{"storage.meta_create.commit_latency"};
monitor::CountRecorder recycleChunkCount{"storage.recycle.count"};
monitor::CountRecorder punchHoleChunkCount{"storage.punch_hole.count"};

enum class MetaKeyType : uint8_t {
  METADATA,
  FREECHUNK,  // deprecated
  FILESIZE,
  LASTSERVE,  // deprecated
  CREATEDSIZE,
  REMOVEDSIZE,
  UNCOMMITTED,
  UNRECYCLED,  // deprecated
  SYNCDUMMY,
  DISABLED = 11,
  REMOVED,
  RECYCLED,
  CREATED,
  HOLE,
  CREATEDCOUNT,
  USEDCOUNT,
  REMOVEDCOUNT,
  RECYCLEDCOUNT,
  REUSEDCOUNT,
  HOLECOUNT,
  ALLOCATEINDEX,
  ALLOCATESTART,
  VERSION,
  MAX = 0xFF,
};

constexpr char kCreatedSizeIns[] = {(char)MetaKeyType::CREATEDSIZE, '\0'};
constexpr auto kCreatedSizeKey = std::string_view{kCreatedSizeIns, 1};

constexpr char kRemovedSizeIns[] = {(char)MetaKeyType::REMOVEDSIZE, '\0'};
constexpr auto kRemovedSizeKey = std::string_view{kRemovedSizeIns, 1};

constexpr char kUnCommittedIns[] = {(char)MetaKeyType::UNCOMMITTED, '\0'};
constexpr auto kUnCommittedKey = std::string_view{kUnCommittedIns, 1};

constexpr char kSyncDummyIns[] = {(char)MetaKeyType::SYNCDUMMY, '\0'};
constexpr auto kSyncDummyKey = std::string_view{kSyncDummyIns, 1};

constexpr char kMinIns[] = {(char)MetaKeyType::METADATA, '\0'};
constexpr auto kMinKey = std::string_view{kMinIns, 1};

constexpr char kMaxIns[] = {(char)MetaKeyType::MAX, '\0'};
constexpr auto kMaxKey = std::string_view{kMaxIns, 1};

// target version list:
// 1. fix removed and recycled count.
constexpr uint32_t kTargetVersion = 1u;

template <auto PREFIX>
class ChunkKey {
 public:
  ChunkKey(std::string_view data)
      : content_(1, char(PREFIX)) {
    reverseBits(data, content_);
  }
  ChunkKey(const ChunkId &id)
      : ChunkKey(id.data()) {}
  operator std::string_view() const { return content_; }
  ChunkId chunkId() const { return chunkId(content_); }

  static ChunkId chunkId(std::string_view view) {
    if (UNLIKELY(view.empty())) {
      return {};
    }
    std::string out;
    reverseBits(view.substr(1), out);
    return ChunkId{std::move(out)};
  }

 private:
  static void reverseBits(std::string_view view, std::string &out) {
    out.reserve(out.size() + view.size());
    for (auto ch : view) {
      out.push_back(~ch);
    }
  }

 private:
  std::string content_;
};

using ChunkMetaKey = ChunkKey<MetaKeyType::METADATA>;
using UncommittedChunkKey = ChunkKey<MetaKeyType::UNCOMMITTED>;

struct FileSizeKey {
  SERDE_CLASS_FIELD(type, MetaKeyType::FILESIZE);
  SERDE_STRUCT_FIELD(fileId, ChunkFileId{});
};

inline auto serializeKey(const auto &o) {
  auto bytes = serde::serializeBytes(o);
  bytes.append(static_cast<uint8_t>(o.type));
  return bytes;
}

Result<Void> deserializeKey(std::string_view view, auto &o) {
  if (UNLIKELY(view.empty() || view[0] != static_cast<char>(o.type))) {
    auto msg = fmt::format("deserialize meta key prefix failed: {:02X}", fmt::join(view.substr(0, 64), ","));
    XLOG(DBG, msg);
    return makeError(StorageCode::kChunkMetadataGetError, std::move(msg));
  }
  return serde::deserialize(o, view.substr(1));
}

template <MetaKeyType Type>
struct ChunkSizePrefix {
  static constexpr auto type = Type;
  SERDE_STRUCT_FIELD(chunkSize, uint32_t{});
};

using AllocateIndexKey = ChunkSizePrefix<MetaKeyType::ALLOCATEINDEX>;
using AllocateStartKey = ChunkSizePrefix<MetaKeyType::ALLOCATESTART>;
using CreatedCountKey = ChunkSizePrefix<MetaKeyType::CREATEDCOUNT>;
using UsedCountKey = ChunkSizePrefix<MetaKeyType::USEDCOUNT>;
using RemovedCountKey = ChunkSizePrefix<MetaKeyType::REMOVEDCOUNT>;
using RecycledCountKey = ChunkSizePrefix<MetaKeyType::RECYCLEDCOUNT>;
using ReusedCountKey = ChunkSizePrefix<MetaKeyType::REUSEDCOUNT>;
using HoleCountKey = ChunkSizePrefix<MetaKeyType::HOLECOUNT>;
using VersionKey = ChunkSizePrefix<MetaKeyType::VERSION>;

struct CreatedKey : public ChunkSizePrefix<MetaKeyType::CREATED> {
  SERDE_STRUCT_FIELD(pos, ChunkPosition{});
};

struct RecycledKey : public ChunkSizePrefix<MetaKeyType::RECYCLED> {
  SERDE_STRUCT_FIELD(pos, ChunkPosition{});
};

struct RemovedKey : public ChunkSizePrefix<MetaKeyType::REMOVED> {
  SERDE_STRUCT_FIELD(microsecond, serde::BigEndian<uint64_t>{});
  SERDE_STRUCT_FIELD(pos, ChunkPosition{});
};

}  // namespace

ChunkMetaStore::~ChunkMetaStore() {
  for (auto &[chunkSize, state] : allocateState_) {
    if (state->loaded) {
      while (state->allocating || state->recycling) {
        XLOGF(WARNING,
              "{} wait async job: allocating {} recycling {}",
              sentinel_,
              state->allocating.load(),
              state->recycling.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }
  }
}

Result<Void> ChunkMetaStore::create(const kv::KVStore::Config &config, const PhysicalConfig &targetConfig) {
  auto createTargetConfig = targetConfig;
  createTargetConfig.has_sentinel = false;
  RETURN_AND_LOG_ON_ERROR(load(config, createTargetConfig, true));
  for (auto chunkSize : targetConfig.chunk_size_list) {
    RETURN_AND_LOG_ON_ERROR(
        kv_->put(serializeKey(VersionKey{uint32_t(chunkSize)}), serde::serializeBytes(kTargetVersion)));
  }
  if (targetConfig.has_sentinel) {
    RETURN_AND_LOG_ON_ERROR(kv_->put(kMinKey, sentinel_));
    RETURN_AND_LOG_ON_ERROR(kv_->put(kMaxKey, sentinel_));
  }
  return Void{};
}

Result<Void> ChunkMetaStore::load(const kv::KVStore::Config &config,
                                  const PhysicalConfig &targetConfig,
                                  bool createIfMissing /* = false */) {
  sentinel_ = fmt::format("3fs-vnext storage target {}", targetConfig.target_id);
  hasSentinel_ = targetConfig.has_sentinel;
  kvName_ = targetConfig.kv_store_name;

  kv_.reset();
  kv::KVStore::Options options;
  options.type = targetConfig.kv_store_type;
  options.path = targetConfig.kvPath();
  options.createIfMissing = createIfMissing;
  kv_ = kv::KVStore::create(config, options);
  if (UNLIKELY(kv_ == nullptr)) {
    XLOGF(ERR, "chunk store init failed");
    return makeError(StorageCode::kChunkStoreInitFailed);
  }
  physicalFileCount_ = targetConfig.physical_file_count;

  if (hasSentinel_) {
    RETURN_AND_LOG_ON_ERROR(checkSentinel(kMinKey));
    RETURN_AND_LOG_ON_ERROR(checkSentinel(kMaxKey));
  }

  RETURN_AND_LOG_ON_ERROR(getSize(kCreatedSizeKey, createdSize_));
  RETURN_AND_LOG_ON_ERROR(getSize(kRemovedSizeKey, removedSize_));

  {
    uncommitted_.clear();
    kv_->iterateKeysWithPrefix(kUnCommittedKey, std::numeric_limits<uint32_t>::max(), [&](std::string_view key, auto) {
      uncommitted_.push_back(UncommittedChunkKey::chunkId(key));
      return Void{};
    });
  }

  for (auto chunkSize : targetConfig.chunk_size_list) {
    createAllocateState(chunkSize);
  }
  return Void{};
}

Result<Void> ChunkMetaStore::addChunkSize(const std::vector<Size> &sizeList) {
  for (auto chunkSize : sizeList) {
    RETURN_AND_LOG_ON_ERROR(
        kv_->put(serializeKey(VersionKey{uint32_t(chunkSize)}), serde::serializeBytes(kTargetVersion)));
    createAllocateState(chunkSize);
    XLOGF(WARNING, "{} chunk meta is initialized, size {}", sentinel_, chunkSize);
  }
  return Void{};
}

Result<Void> ChunkMetaStore::migrate(const kv::KVStore::Config &config, const PhysicalConfig &targetConfig) {
  if (UNLIKELY(targetConfig.kv_store_name == kvName_)) {
    auto msg = fmt::format("meta store cannot migrate from {} to {}", kvName_, targetConfig.kv_store_name);
    XLOG(ERR, msg);
    return makeError(StorageCode::kMetaStoreInvalidIterator, std::move(msg));
  }
  Path newPath = targetConfig.kvPath();
  if (boost::filesystem::exists(newPath)) {
    boost::system::error_code ec{};
    Path movePath = targetConfig.path / fmt::format("{}.{}", targetConfig.kv_store_name, UtcClock::now());
    boost::filesystem::rename(newPath, movePath, ec);
    if (UNLIKELY(ec.failed())) {
      auto msg = fmt::format("meta store cannot migrate move {} to {} failed: {}", newPath, movePath, ec.message());
      XLOG(ERR, msg);
      return makeError(StorageCode::kMetaStoreInvalidIterator, std::move(msg));
    }
  }

  // 1. create old iterator.
  auto oldIt = kv_->createIterator();
  if (UNLIKELY(!oldIt)) {
    auto msg = "meta store create iterator failed";
    XLOG(ERR, msg);
    return makeError(StorageCode::kMetaStoreInvalidIterator, msg);
  }

  // 2. put all key-value into new kv.
  kv::KVStore::Options options;
  options.type = targetConfig.kv_store_type;
  options.path = targetConfig.kvPath();
  options.createIfMissing = true;
  auto newKV = kv::KVStore::create(config, options);
  for (oldIt->seekToFirst(); oldIt->valid(); oldIt->next()) {
    RETURN_AND_LOG_ON_ERROR(newKV->put(oldIt->key(), oldIt->value()));
  }
  RETURN_AND_LOG_ON_ERROR(oldIt->status());

  // 3. check all key-value.
  auto newIt = newKV->createIterator();
  if (UNLIKELY(!newIt)) {
    auto msg = "meta store create iterator failed";
    XLOG(ERR, msg);
    return makeError(StorageCode::kMetaStoreInvalidIterator, msg);
  }
  for (oldIt->seekToFirst(), newIt->seekToFirst(); oldIt->valid() && newIt->valid(); oldIt->next(), newIt->next()) {
    if (UNLIKELY(oldIt->key() != newIt->key() || oldIt->value() != newIt->value())) {
      auto msg = fmt::format("meta store migrate failed old {:02X}:{:02X} != new {:02X}:{:02X}",
                             fmt::join(oldIt->key().substr(0, 64), ","),
                             fmt::join(oldIt->value().substr(0, 64), ","),
                             fmt::join(newIt->key().substr(0, 64), ","),
                             fmt::join(newIt->value().substr(0, 64), ","));
      XLOG(ERR, msg);
      return makeError(StorageCode::kMetaStoreInvalidIterator, msg);
    }
  }
  RETURN_AND_LOG_ON_ERROR(oldIt->status());
  RETURN_AND_LOG_ON_ERROR(newIt->status());
  if (UNLIKELY(oldIt->valid() || newIt->valid())) {
    auto &it = oldIt->valid() ? oldIt : newIt;
    auto msg = fmt::format("meta store migrate failed {} {:02X}:{:02X}",
                           oldIt->valid() ? "old" : "new",
                           fmt::join(it->key().substr(0, 64), ","),
                           fmt::join(it->value().substr(0, 64), ","));
    XLOG(ERR, msg);
    return makeError(StorageCode::kMetaStoreInvalidIterator, msg);
  }
  if (!hasSentinel_ && targetConfig.has_sentinel) {
    RETURN_AND_LOG_ON_ERROR(newKV->put(kMinKey, sentinel_));
    RETURN_AND_LOG_ON_ERROR(newKV->put(kMaxKey, sentinel_));
    hasSentinel_ = true;
  }
  kvName_ = targetConfig.kv_store_name;
  oldIt.reset();
  kv_.reset();
  kv_ = std::move(newKV);
  return Void{};
}

Result<Void> ChunkMetaStore::get(const ChunkId &chunkId, ChunkMetadata &meta) {
  auto guard = storageMetaGet.record();

  ChunkMetaKey key(chunkId);
  auto result = kv_->get(key);
  if (!result) {
    if (LIKELY(result.error().code() == StatusCode::kKVStoreNotFound)) {
      auto msg = fmt::format("chunk id {} meta not found: {}", chunkId, result.error());
      XLOG(DBG, msg);
      return makeError(StorageCode::kChunkMetadataNotFound, std::move(msg));
    }
    auto msg = fmt::format("chunk id {} get meta error: {}", chunkId, result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataGetError, std::move(msg));
  }

  RETURN_AND_LOG_ON_ERROR(serde::deserialize(meta, result.value()));

  guard.succ();
  return Void{};
}

Result<Void> ChunkMetaStore::set(const ChunkId &chunkId, const ChunkMetadata &meta) {
  auto guard = storageMetaSet.record();

  auto batchOp = kv_->createBatchOps();

  // 1. put metadata.
  batchOp->put(ChunkMetaKey(chunkId), serde::serializeBytes(meta));

  // 2. commit chunk.
  if (meta.chunkState == ChunkState::COMMIT) {
    batchOp->remove(UncommittedChunkKey(chunkId));
  } else {
    batchOp->put(UncommittedChunkKey(chunkId), {});
  }

  auto result = batchOp->commit();
  if (UNLIKELY(!result)) {
    auto msg = fmt::format("chunk id {} set meta error: {}", chunkId, result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataSetError, std::move(msg));
  }

  guard.succ();
  return Void{};
}

Result<Void> ChunkMetaStore::remove(const ChunkId &chunkId, const ChunkMetadata &meta) {
  auto guard = storageMetaRemove.record();

  auto stateResult = loadAllocateState(meta.innerFileId.chunkSize);
  RETURN_AND_LOG_ON_ERROR(stateResult);
  auto &state = **stateResult;

  auto now = UtcClock::now();
  auto batchOp = kv_->createBatchOps();

  // 1. remove metadata.
  ChunkMetaKey chunkMetaKey(chunkId);
  XLOGF(DBG, "remove meta data {}", meta);
  batchOp->remove(chunkMetaKey);
  batchOp->remove(UncommittedChunkKey(chunkId));

  const bool doPunchHole = meta.innerOffset < state.startingPoint.load();
  if (doPunchHole) {
    RETURN_AND_LOG_ON_ERROR(fileStore_.punchHole(meta.innerFileId, meta.innerOffset));
  } else {
    // 2. insert remove key.
    RemovedKey removedKey;
    removedKey.chunkSize = meta.innerFileId.chunkSize;
    removedKey.microsecond = now.toMicroseconds();
    removedKey.pos = {meta.innerFileId.chunkIdx, {meta.innerOffset}};
    batchOp->put(serializeKey(removedKey), {});

    // 3. update removed count.
    RemovedCountKey removedCountKey{state.chunkSize};
    batchOp->put(serializeKey(removedCountKey), serde::serializeBytes(++state.removedCount));
  }

  // 4. update removed size.
  removedSize_ += meta.innerFileId.chunkSize;
  batchOp->put(kRemovedSizeKey, serde::serializeBytes(removedSize_.load()));

  auto result = batchOp->commit();
  if (UNLIKELY(!result)) {
    if (!doPunchHole) {
      --state.removedCount;
    }
    removedSize_ -= meta.innerFileId.chunkSize;
    auto msg = fmt::format("chunk id {} remote failed: {}", chunkId, result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataSetError, std::move(msg));
  }

  if (state.oldestRemovedTimestamp.load().isZero()) {
    state.oldestRemovedTimestamp = now;
  }
  guard.succ();
  return Void{};
}

Result<Void> ChunkMetaStore::createChunk(const ChunkId &chunkId,
                                         ChunkMetadata &meta,
                                         uint32_t chunkSize,
                                         folly::CPUThreadPoolExecutor &executor,
                                         bool allowToAllocate) {
  auto recordGuard = storageMetaCreate.record();

  auto stateResult = loadAllocateState(chunkSize);
  RETURN_AND_LOG_ON_ERROR(stateResult);
  auto &state = **stateResult;

  ChunkPosition pos;
  bool useRecycledChunk = false;
  auto batchOp = kv_->createBatchOps();

  {
    auto startTime = RelativeTime::now();
    auto lock = std::unique_lock(state.createMutex);
    storageMetaCreateWait.addSample(RelativeTime::now() - startTime);
    if (needRecycleRemovedChunks(state)) {
      if (!state.recycling.exchange(true)) {
        executor.add([this, &state] { recycleRemovedChunks(state); });
      }
      if (state.recycledChunks.empty()) {
        lock.unlock();
        auto recycleLock = std::unique_lock(state.recycleMutex);
        lock.lock();
        if (state.recycledChunks.empty()) {
          RETURN_AND_LOG_ON_ERROR(recycleRemovedChunks(state, true));
        }
      }
    }
    useRecycledChunk = !state.recycledChunks.empty();
    if (useRecycledChunk) {
      // 2.1 use recycled chunks.
      pos = state.recycledChunks.back();
      state.recycledChunks.pop_back();
      batchOp->put(serializeKey(ReusedCountKey{state.chunkSize}), serde::serializeBytes(++state.reusedCount));

      RecycledKey recycledKey;
      recycledKey.chunkSize = chunkSize;
      recycledKey.pos = pos;
      batchOp->remove(serializeKey(recycledKey));
    } else {
      if (!allowToAllocate) {
        auto msg = fmt::format("chunk {} create new chunk write limit", chunkId);
        XLOG(ERR, msg);
        return makeError(StorageClientCode::kNoSpace, std::move(msg));
      }

      // 2.2 use created chunks.
      if (state.createdChunks.size() * state.chunkSize <= config_.allocate_size() / 2) {
        if (!state.allocating.exchange(true)) {
          executor.add([this, &state] { allocateChunks(state); });
        }
      }
      if (state.createdChunks.empty()) {
        lock.unlock();
        auto allocateLock = std::unique_lock(state.allocateMutex);
        lock.lock();
        if (state.createdChunks.empty()) {
          RETURN_AND_LOG_ON_ERROR(allocateChunks(state, true));
        }
      }
      pos = state.createdChunks.back();
      state.createdChunks.pop_back();
      batchOp->put(serializeKey(UsedCountKey{state.chunkSize}), serde::serializeBytes(++state.usedCount));

      CreatedKey extendKey;
      extendKey.chunkSize = chunkSize;
      extendKey.pos = pos;
      batchOp->remove(serializeKey(extendKey));
    }
  }

  // 3. insert metadata.
  meta.innerFileId = {chunkSize, pos.fileIdx};
  meta.innerOffset = size_t{pos.offset};
  ChunkMetaKey chunkMetaKey(chunkId);
  XLOGF(DBG, "insert meta data {}", meta);
  batchOp->put(chunkMetaKey, serde::serializeBytes(meta));

  // 4. update created size.
  batchOp->put(kCreatedSizeKey, serde::serializeBytes(createdSize_ += chunkSize));

  // 5. commit on kv.
  auto startTime = RelativeTime::now();
  auto result = batchOp->commit();
  storageMetaCreateCommit.addSample(RelativeTime::now() - startTime);
  if (UNLIKELY(!result)) {
    createdSize_ -= chunkSize;
    {
      auto lock = std::unique_lock(state.createMutex);
      if (useRecycledChunk) {
        state.recycledChunks.push_back(pos);
        --state.reusedCount;
      } else {
        state.createdChunks.push_back(pos);
        --state.usedCount;
      }
    }
    auto msg = fmt::format("chunk id {} create failed: {}", chunkId, result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataSetError, std::move(msg));
  }

  recordGuard.succ();
  return Void{};
}

Result<bool> ChunkMetaStore::punchHole() {
  bool punchAll = true;
  auto expirationUs = UtcClock::now().toMicroseconds() - config_.removed_chunk_expiration_time().asUs().count();
  if (emergencyRecycling_) {
    expirationUs = UtcClock::now().toMicroseconds();
  }
  for (auto &[chunkSize, state] : allocateState_) {
    // 1. load state.
    if (UNLIKELY(!state->loaded)) {
      RETURN_AND_LOG_ON_ERROR(loadAllocateState(chunkSize));
    }

    // 2. check oldest removed chunk timestamp.
    auto oldestRemovedTimestamp = state->oldestRemovedTimestamp.load();
    if (oldestRemovedTimestamp.isZero() || oldestRemovedTimestamp.toMicroseconds() >= expirationUs) {
      continue;
    }

    // 3. punch hole.
    auto result = punchHoleRemovedChunks(*state, expirationUs);
    RETURN_AND_LOG_ON_ERROR(result);
    punchAll &= *result;
  }
  return punchAll;
}

Result<Void> ChunkMetaStore::sync() { return kv_->put(kSyncDummyKey, {}, /* sync = */ true); }

Result<Void> ChunkMetaStore::unusedSize(int64_t &reservedSize, int64_t &unrecycledSize) {
  for (auto &[chunkSize, state] : allocateState_) {
    // 1. load state.
    if (UNLIKELY(!state->loaded)) {
      RETURN_AND_LOG_ON_ERROR(loadAllocateState(chunkSize));
    }

    // 2. calculate unrecycled size.
    const int64_t createdCount = state->createdCount;
    const int64_t usedCount = state->usedCount;
    const int64_t removedCount = state->removedCount;
    const int64_t recycledCount = state->recycledCount;
    const int64_t reusedCount = state->reusedCount;
    const int64_t holeCount = state->holeCount;
    const int64_t reservedCount = std::max(0l, createdCount - usedCount) + std::max(0l, recycledCount - reusedCount);
    const int64_t unrecycledCount = std::max(0l, removedCount - recycledCount - holeCount);
    reservedSize += chunkSize * reservedCount;
    unrecycledSize += chunkSize * unrecycledCount;
  }
  return Void{};
}

ChunkMetaStore::Iterator::Iterator(kv::KVStore::IteratorPtr it, std::string_view chunkIdPrefix)
    : it_(std::move(it)) {
  seek(chunkIdPrefix);
}
// seek a chunk id prefix.
void ChunkMetaStore::Iterator::seek(std::string_view chunkIdPrefix) {
  it_->seek(ChunkMetaKey(chunkIdPrefix));
  if (it_->valid() && it_->key() == kMinKey) {
    it_->next();
  }
}
// return valid or not.
bool ChunkMetaStore::Iterator::valid() const {
  return it_->valid() && it_->key().starts_with(char(MetaKeyType::METADATA));
}
// get current chunk id.
ChunkId ChunkMetaStore::Iterator::chunkId() const { return ChunkMetaKey::chunkId(it_->key()); }
// get current metadata.
Result<ChunkMetadata> ChunkMetaStore::Iterator::meta() const {
  ChunkMetadata meta;
  RETURN_AND_LOG_ON_ERROR(serde::deserialize(meta, it_->value()));
  return meta;
}
// next metadata.
void ChunkMetaStore::Iterator::next() { return it_->next(); }
// check status.
Result<Void> ChunkMetaStore::Iterator::status() const { return it_->status(); }

Result<ChunkMetaStore::Iterator> ChunkMetaStore::iterator(std::string_view chunkIdPrefix) {
  auto it = kv_->createIterator();
  if (UNLIKELY(!it)) {
    auto msg = "meta store create iterator failed";
    XLOG(ERR, msg);
    return makeError(StorageCode::kMetaStoreInvalidIterator, msg);
  }
  return Iterator(std::move(it), chunkIdPrefix);
}

Result<Void> ChunkMetaStore::checkSentinel(std::string_view key) {
  auto result = kv_->get(key);
  RETURN_AND_LOG_ON_ERROR(result);
  if (*result != sentinel_) {
    auto msg = fmt::format("meta store check sentinel failed, sentinel: {}", sentinel_);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StorageCode::kMetaStoreOpenFailed, msg);
  }
  return Void{};
}

Result<Void> ChunkMetaStore::getSize(std::string_view key, std::atomic<uint64_t> &size) {
  auto result = kv_->get(key);
  if (LIKELY(bool(result))) {
    uint64_t value;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(value, *result));
    size = value;
  } else if (result.error().code() == StatusCode::kKVStoreNotFound) {
    // it's ok.
    size = 0;
  } else {
    RETURN_AND_LOG_ON_ERROR(result);
  }
  return Void{};
}

void ChunkMetaStore::createAllocateState(uint32_t chunkSize) {
  auto state = std::make_unique<AllocateState>();
  state->chunkSize = chunkSize;
  allocateState_.emplace(chunkSize, std::move(state));
}

Result<ChunkMetaStore::AllocateState *> ChunkMetaStore::loadAllocateState(uint32_t chunkSize) {
  auto it = allocateState_.find(chunkSize);
  if (UNLIKELY(it == allocateState_.end())) {
    return makeError(StorageCode::kChunkInvalidChunkSize, fmt::format("invalid chunk size {}", chunkSize));
  }
  auto &state = *it->second;
  if (state.loaded) {
    return &state;
  }

  auto lock = std::unique_lock(state.createMutex);
  if (state.loaded) {
    return &state;
  }

  // 0. load target version.
  uint32_t version = 0;
  auto versionKeyStr = serde::serializeBytes(VersionKey{chunkSize});
  auto versionResult = kv_->get(versionKeyStr);
  if (LIKELY(bool(versionResult))) {
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(version, *versionResult));
  } else if (versionResult.error().code() == StatusCode::kKVStoreNotFound) {
    // it's ok and init with 0.
  } else {
    RETURN_AND_LOG_ON_ERROR(versionResult);
  }

  // 1. load allocate index.
  auto result = kv_->get(serializeKey(AllocateIndexKey{state.chunkSize}));
  if (LIKELY(bool(result))) {
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(state.allocateIndex, *result));
  } else if (result.error().code() == StatusCode::kKVStoreNotFound) {
    // it's ok and init with 0.
  } else {
    RETURN_AND_LOG_ON_ERROR(result);
  }
  auto allocateStartKey = serializeKey(AllocateStartKey{state.chunkSize});
  result = kv_->get(allocateStartKey);
  if (LIKELY(bool(result))) {
    uint64_t startingPoint{};
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(startingPoint, *result));
    state.startingPoint = startingPoint;
  } else if (result.error().code() == StatusCode::kKVStoreNotFound) {
    uint64_t fileSize{};
    FileSizeKey fileSizeKey;
    fileSizeKey.fileId = ChunkFileId{state.chunkSize, 0};
    auto fileSizeKeyStr = serde::serializeBytes(fileSizeKey);
    auto result = kv_->get(fileSizeKeyStr);
    if (LIKELY(bool(result))) {
      RETURN_AND_LOG_ON_ERROR(serde::deserialize(fileSize, *result));
    } else if (result.error().code() == StatusCode::kKVStoreNotFound) {
      // it's ok and init with 0.
    } else {
      RETURN_AND_LOG_ON_ERROR(result);
    }
    auto allocateSize = config_.allocate_size();
    state.startingPoint = (fileSize + allocateSize - 1) / allocateSize * allocateSize;
    RETURN_AND_LOG_ON_ERROR(kv_->put(allocateStartKey, serde::serializeBytes(state.startingPoint.load())));
  } else {
    RETURN_AND_LOG_ON_ERROR(result);
  }

  if (version == 0) {
    // version 0->1: fix removed and recycled count.
    auto batchOp = kv_->createBatchOps();
    version = 1;
    batchOp->put(versionKeyStr, serde::serializeBytes(version));
    if (state.startingPoint > 0) {
      uint64_t removedCount = 0;
      uint64_t recycledCount = 0;
      // scan removed key and re-count it.
      RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
          serializeKey(ChunkSizePrefix<MetaKeyType::REMOVED>{state.chunkSize}),
          std::numeric_limits<uint32_t>::max(),
          [&](std::string_view key, auto) -> Result<Void> {
            RemovedKey removedKey;
            RETURN_AND_LOG_ON_ERROR(deserializeKey(key, removedKey));
            if (removedKey.pos.offset < state.startingPoint.load()) {
              // remove it.
              batchOp->remove(key);
            } else {
              ++removedCount;
            }
            return Void{};
          },
          nullptr));
      // scan recycled key and re-count it.
      RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
          serializeKey(ChunkSizePrefix<MetaKeyType::RECYCLED>{chunkSize}),
          std::numeric_limits<uint32_t>::max(),
          [&](std::string_view key, auto) -> Result<Void> {
            RecycledKey recycledKey;
            RETURN_AND_LOG_ON_ERROR(deserializeKey(key, recycledKey));
            if (recycledKey.pos.offset < state.startingPoint.load()) {
              // remove it.
              batchOp->remove(key);
            } else {
              ++recycledCount;
              ++removedCount;
            }
            return Void{};
          },
          nullptr));

      batchOp->put(serializeKey(RemovedCountKey{chunkSize}), serde::serializeBytes(removedCount));
      batchOp->put(serializeKey(RecycledCountKey{chunkSize}), serde::serializeBytes(recycledCount));
      batchOp->put(serializeKey(ReusedCountKey{chunkSize}), serde::serializeBytes(uint64_t{}));
      batchOp->put(serializeKey(HoleCountKey{chunkSize}), serde::serializeBytes(uint64_t{}));
      XLOGF(WARNING, "chunk size {} version 1 fix removed {} recycled {}", chunkSize, removedCount, recycledCount);
    }

    RETURN_AND_LOG_ON_ERROR(batchOp->commit());
  }

  // 2. load created chunks.
  std::vector<ChunkPosition> createdChunks;
  RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
      serializeKey(ChunkSizePrefix<MetaKeyType::CREATED>{chunkSize}),
      std::numeric_limits<uint32_t>::max(),
      [&](std::string_view key, auto) -> Result<Void> {
        CreatedKey createdKey;
        RETURN_AND_LOG_ON_ERROR(deserializeKey(key, createdKey));
        createdChunks.push_back(createdKey.pos);
        XLOGF(DBG5, "load created key: {}", createdKey);
        return Void{};
      },
      nullptr));
  std::reverse(createdChunks.begin(), createdChunks.end());

  // 3. load unused chunks.
  std::vector<ChunkPosition> recycledChunks;
  RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
      serializeKey(ChunkSizePrefix<MetaKeyType::RECYCLED>{chunkSize}),
      std::numeric_limits<uint32_t>::max(),
      [&](std::string_view key, auto) -> Result<Void> {
        RecycledKey recycledKey;
        RETURN_AND_LOG_ON_ERROR(deserializeKey(key, recycledKey));
        recycledChunks.push_back(recycledKey.pos);
        XLOGF(DBG5, "load recycled key: {}", recycledKey);
        return Void{};
      },
      nullptr));

  // 4. load size.
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(CreatedCountKey{state.chunkSize}), state.createdCount));
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(UsedCountKey{state.chunkSize}), state.usedCount));
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(RemovedCountKey{state.chunkSize}), state.removedCount));
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(RecycledCountKey{state.chunkSize}), state.recycledCount));
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(ReusedCountKey{state.chunkSize}), state.reusedCount));
  RETURN_AND_LOG_ON_ERROR(getSize(serializeKey(HoleCountKey{state.chunkSize}), state.holeCount));

  // 5. load oldest removed timestamp.
  std::optional<std::string> nextValidKey;
  RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
      serializeKey(ChunkSizePrefix<MetaKeyType::REMOVED>{state.chunkSize}),
      0,
      [](auto, auto) -> Result<Void> { return Void{}; },
      &nextValidKey));
  UtcTime oldestRemovedTimestamp{};
  if (nextValidKey) {
    RemovedKey removedKey;
    RETURN_AND_LOG_ON_ERROR(deserializeKey(*nextValidKey, removedKey));
    oldestRemovedTimestamp = UtcTime::fromMicroseconds(removedKey.microsecond);
  }

  state.recycledChunks = std::move(recycledChunks);
  state.createdChunks = std::move(createdChunks);
  state.oldestRemovedTimestamp = oldestRemovedTimestamp;
  state.loaded = true;
  return &state;
}

Result<Void> ChunkMetaStore::allocateChunks(AllocateState &state, bool withLock /* = false */) {
  auto guard = folly::makeGuard([&, withLock] {
    if (!withLock) {
      state.allocating = false;
    }
  });
  std::unique_lock<std::mutex> allocateLock;
  if (!withLock) {
    allocateLock = std::unique_lock(state.allocateMutex);
  }

  // 1. get allocate file id.
  auto nextAllocateIndex = (state.allocateIndex + 1) % physicalFileCount_;

  // 2. get file size.
  FileSizeKey fileSizeKey;
  fileSizeKey.fileId = ChunkFileId{state.chunkSize, state.allocateIndex};
  auto fileSizeKeyStr = serde::serializeBytes(fileSizeKey);
  auto [it, succ] = state.fileSize.emplace(state.allocateIndex, 0);
  auto &fileSize = it->second;
  if (succ) {
    // not found in map, then get from KV.
    auto result = kv_->get(fileSizeKeyStr);
    if (LIKELY(bool(result))) {
      RETURN_AND_LOG_ON_ERROR(serde::deserialize(fileSize, *result));
      if (fileSize < state.startingPoint) {
        fileSize = state.startingPoint;
      }
    } else if (result.error().code() == StatusCode::kKVStoreNotFound) {
      // it's ok and init with 0.
    } else {
      RETURN_AND_LOG_ON_ERROR(result);
    }
  }
  if (UNLIKELY(fileSize % state.chunkSize != 0)) {
    auto msg = fmt::format("file info size not aligned: {} chunk size {}",
                           Size::toString(fileSize),
                           Size::toString(state.chunkSize));
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataGetError, std::move(msg));
  }

  // 3. allocate space.
  auto allocateSize = config_.allocate_size();
  auto newFileSize = (fileSize / allocateSize + 1) * allocateSize;

  RETURN_AND_LOG_ON_ERROR(
      fileStore_.allocate(ChunkFileId{state.chunkSize, state.allocateIndex}, fileSize, newFileSize - fileSize));

  // 4. commit it.
  auto batchOp = kv_->createBatchOps();
  batchOp->put(serializeKey(AllocateIndexKey{state.chunkSize}), serde::serializeBytes(nextAllocateIndex));
  batchOp->put(fileSizeKeyStr, serde::serializeBytes(newFileSize));
  std::vector<ChunkPosition> createdChunks;
  for (auto offset = fileSize; offset != newFileSize; offset += state.chunkSize) {
    CreatedKey key;
    key.chunkSize = state.chunkSize;
    key.pos = {state.allocateIndex, offset};
    batchOp->put(serializeKey(key), {});
    XLOGF(DBG5, "create created key: {}", key);
    createdChunks.push_back(ChunkPosition{state.allocateIndex, offset});
  }
  std::reverse(createdChunks.begin(), createdChunks.end());
  auto createdCount = state.createdCount + createdChunks.size();
  batchOp->put(serializeKey(CreatedCountKey{state.chunkSize}), serde::serializeBytes(createdCount));
  auto result = batchOp->commit();
  RETURN_AND_LOG_ON_ERROR(result);
  guard.dismiss();

  std::unique_lock<std::mutex> createLock;
  if (!withLock) {
    createLock = std::unique_lock(state.createMutex);
  }
  state.allocateIndex = nextAllocateIndex;
  if (state.createdChunks.empty()) {
    state.createdChunks = std::move(createdChunks);
  } else {
    state.createdChunks.insert(state.createdChunks.end(), createdChunks.begin(), createdChunks.end());
  }
  state.createdCount = createdCount;
  fileSize = newFileSize;
  if (!withLock) {
    state.allocating = false;
  }
  return Void{};
}

bool ChunkMetaStore::needRecycleRemovedChunks(AllocateState &state) {
  if (state.recycling) {
    return false;
  }

  if (state.recycledChunks.size() >= config_.recycle_batch_size() * 0.6) {
    return false;
  }

  const auto removedCount = state.removedCount.load();
  const auto recycledAndHoleCount = state.recycledCount + state.holeCount;
  const bool doRecycle =
      recycledAndHoleCount + config_.recycle_batch_size() <= removedCount ||
      (recycledAndHoleCount < removedCount &&
       state.oldestRemovedTimestamp.load() + config_.removed_chunk_force_recycled_time() < UtcClock::now());
  return doRecycle;
}

Result<Void> ChunkMetaStore::recycleRemovedChunks(AllocateState &state, bool withLock /* = false */) {
  auto guard = folly::makeGuard([&, withLock] {
    if (!withLock) {
      state.recycling = false;
    }
  });
  auto recordGuard = storageMetaRecycle.record();
  std::unique_lock<std::mutex> lock;
  if (!withLock) {
    lock = std::unique_lock(state.recycleMutex);
  }

  auto batchOp = kv_->createBatchOps();
  auto recycleBatchSize = config_.recycle_batch_size();
  std::vector<ChunkPosition> recycledChunks;
  std::optional<std::string> nextValidKey;
  RETURN_AND_LOG_ON_ERROR(kv_->iterateKeysWithPrefix(
      serializeKey(ChunkSizePrefix<MetaKeyType::REMOVED>{state.chunkSize}),
      recycleBatchSize,
      [&](std::string_view key, auto) -> Result<Void> {
        RemovedKey removedKey;
        RETURN_AND_LOG_ON_ERROR(deserializeKey(key, removedKey));
        recycledChunks.push_back(removedKey.pos);
        batchOp->remove(key);
        RecycledKey recycledKey;
        recycledKey.chunkSize = state.chunkSize;
        recycledKey.pos = removedKey.pos;
        batchOp->put(serializeKey(recycledKey), {});
        XLOGF(DBG5, "create recycled key: {}", recycledKey);
        return Void{};
      },
      &nextValidKey));
  UtcTime oldestRemovedTimestamp{};
  if (nextValidKey) {
    RemovedKey removedKey;
    RETURN_AND_LOG_ON_ERROR(deserializeKey(*nextValidKey, removedKey));
    oldestRemovedTimestamp = UtcTime::fromMicroseconds(removedKey.microsecond);
  }

  if (recycledChunks.empty()) {
    recordGuard.succ();
    return Void{};
  }

  batchOp->put(serializeKey(RecycledCountKey{state.chunkSize}),
               serde::serializeBytes(state.recycledCount += recycledChunks.size()));

  auto result = batchOp->commit();
  if (UNLIKELY(!result)) {
    state.recycledCount -= recycledChunks.size();
    auto msg = fmt::format("reuse removed chunk submit failed, error {}", result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataSetError, std::move(msg));
  }
  recycleChunkCount.addSample(recycledChunks.size());
  state.oldestRemovedTimestamp = oldestRemovedTimestamp;
  recordGuard.succ();
  guard.dismiss();

  std::unique_lock<std::mutex> createLock;
  if (!withLock) {
    createLock = std::unique_lock(state.createMutex);
  }
  if (state.recycledChunks.empty()) {
    state.recycledChunks = std::move(recycledChunks);
  } else {
    state.recycledChunks.insert(state.recycledChunks.end(), recycledChunks.begin(), recycledChunks.end());
  }
  if (!withLock) {
    state.recycling = false;
  }
  return Void{};
}

Result<bool> ChunkMetaStore::punchHoleRemovedChunks(AllocateState &state, uint64_t expirationUs) {
  auto lock = std::unique_lock(state.recycleMutex);

  auto batchOp = kv_->createBatchOps();
  auto punchHoleBatchSize = config_.punch_hole_batch_size();
  uint32_t punchHoleCount = 0;
  UtcTime oldestRemovedTimestamp{};
  auto iterateResult = kv_->iterateKeysWithPrefix(
      serializeKey(ChunkSizePrefix<MetaKeyType::REMOVED>{state.chunkSize}),
      std::numeric_limits<uint32_t>::max(),
      [&](std::string_view key, auto) -> Result<Void> {
        RemovedKey removedKey;
        RETURN_AND_LOG_ON_ERROR(deserializeKey(key, removedKey));
        if (removedKey.microsecond > expirationUs || punchHoleCount >= punchHoleBatchSize) {
          oldestRemovedTimestamp = UtcTime::fromMicroseconds(removedKey.microsecond);
          return makeError(Status::OK);
        }
        batchOp->remove(key);
        RETURN_AND_LOG_ON_ERROR(
            fileStore_.punchHole(ChunkFileId{removedKey.chunkSize, removedKey.pos.fileIdx}, removedKey.pos.offset));
        ++punchHoleCount;
        return Void{};
      },
      nullptr);
  if (iterateResult.hasError() && iterateResult.error().code() != StatusCode::kOK) {
    RETURN_AND_LOG_ON_ERROR(iterateResult);
  }
  if (punchHoleCount == 0) {
    state.oldestRemovedTimestamp = oldestRemovedTimestamp;
    return true;
  }

  batchOp->put(serializeKey(HoleCountKey{state.chunkSize}), serde::serializeBytes(state.holeCount += punchHoleCount));

  auto result = batchOp->commit();
  if (UNLIKELY(!result)) {
    state.holeCount -= punchHoleCount;
    auto msg = fmt::format("punch hole removed chunk submit failed, error {}", result.error());
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkMetadataSetError, std::move(msg));
  }
  punchHoleChunkCount.addSample(punchHoleCount);
  state.oldestRemovedTimestamp = oldestRemovedTimestamp;
  return oldestRemovedTimestamp.isZero();  // no more valid key.
}

}  // namespace hf3fs::storage
