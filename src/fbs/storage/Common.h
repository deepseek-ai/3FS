#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <folly/hash/Checksum.h>
#include <iterator>
#include <string>
#include <vector>

#include "client/mgmtd/RoutingInfo.h"
#include "common/app/ClientId.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/Reflection.h"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/NodeInfo.h"

namespace hf3fs::storage {
using ChainId = ::hf3fs::flat::ChainId;
using ChainVer = ::hf3fs::flat::ChainVersion;
using TargetId = ::hf3fs::flat::TargetId;
using NodeId = ::hf3fs::flat::NodeId;

STRONG_TYPEDEF(uint32_t, ChunkVer);
STRONG_TYPEDEF(uint64_t, RequestId);
STRONG_TYPEDEF(uint16_t, ChannelId);
STRONG_TYPEDEF(uint64_t, ChannelSeqNum);

#define BITFLAGS_SET(x, bits) ((x) |= static_cast<uint32_t>(bits))
#define BITFLAGS_CLEAR(x, bits) ((x) &= (~static_cast<uint32_t>(bits)))
#define BITFLAGS_CONTAIN(x, bits) (((x) & static_cast<uint32_t>(bits)) == static_cast<uint32_t>(bits))

#define ALIGN_LOWER(mem, align) ((mem) / (align) * (align))
#define ALIGN_UPPER(mem, align) (((mem) + (align)-1) / (align) * (align))

/* start of fault injection helpers */

#ifndef NDEBUG
#define FAULT_INJECTION_POINT(injectError, injectedRes, funcRes) \
  (UNLIKELY(injectError) ? (XLOGF(WARN, "Injected error: {}", (injectedRes)), (injectedRes)) : (funcRes))
#else
#define FAULT_INJECTION_POINT(injectError, injectedRes, funcRes) (boost::ignore_unused(injectError), (funcRes))
#endif

/* end of fault injection helpers */

enum class UpdateType : uint8_t {
  INVALID = 0,
  WRITE = 1,
  REMOVE = 2,
  TRUNCATE = 4,
  EXTEND = 8,
  COMMIT = 16,
};

enum class ChunkState : uint8_t {
  COMMIT = 0,
  DIRTY = 1,
  CLEAN = 2,
};

enum class ChecksumType : uint8_t {
  NONE = 0,
  CRC32C = 1,
  CRC32 = 2,
};

enum class FeatureFlags : uint32_t {
  DEFAULT = 0,
  BYPASS_DISKIO = 1,
  BYPASS_RDMAXMIT = 2,
  SEND_DATA_INLINE = 4,
  ALLOW_READ_UNCOMMITTED = 8,
};

constexpr auto kAIOAlignSize = 4096ul;

class ChunkId {
 public:
  ChunkId() = default;
  explicit ChunkId(const std::string &data)
      : data_(data) {}
  explicit ChunkId(std::string_view data)
      : data_(data) {}
  explicit ChunkId(std::string &&data)
      : data_(std::move(data)) {}

  // special constructors to create 128-bit chunk id from 64-bit integers
  ChunkId(uint64_t high, uint64_t low);
  ChunkId(const ChunkId &baseChunkId, uint64_t chunkIndex);

  bool operator==(const ChunkId &other) const { return other.data_ == data_; }
  auto operator<=>(const ChunkId &other) const { return data_ <=> other.data_; }

  ChunkId nextChunkId() const;

  ChunkId rangeEndForCurrentChunk() const;

  auto &data() const { return data_; }
  std::string describe() const;
  std::string toString() const { return describe(); }
  static Result<ChunkId> fromString(std::string_view str);

 private:
  std::string data_;
};
static_assert(serde::Serializable<ChunkId>);

struct ChecksumInfo {
  SERDE_STRUCT_FIELD(type, ChecksumType::NONE);
  SERDE_STRUCT_FIELD(value, uint32_t{});

 public:
  static constexpr size_t kChunkSize = 1_MB;

  class DataIterator {
   public:
    virtual ~DataIterator() = default;
    virtual std::pair<const uint8_t *, size_t> next() = 0;
  };

  class MemoryDataIterator : public DataIterator {
   public:
    MemoryDataIterator(const uint8_t *buffer, size_t length)
        : buffer_(buffer),
          length_(length) {}

    std::pair<const uint8_t *, size_t> next() override {
      if (length_ == 0) return {nullptr, 0};
      const uint8_t *data = buffer_;
      size_t size = std::min(length_, ChecksumInfo::kChunkSize);
      buffer_ += size;
      length_ -= size;
      return {data, size};
    }

   private:
    const uint8_t *buffer_;
    size_t length_;
  };

  static ChecksumInfo create(ChecksumType type, DataIterator *iter, size_t length, uint32_t startingChecksum = ~0U) {
    ChecksumInfo checksum = {type, startingChecksum};
    size_t iterBytes = 0;

    if (type == ChecksumType::NONE) return ChecksumInfo{ChecksumType::NONE, 0U};

    for (auto data = iter->next(); data.first != nullptr && iterBytes < length; data = iter->next()) {
      iterBytes += data.second;
      switch (checksum.type) {
        case ChecksumType::NONE:
          break;
        case ChecksumType::CRC32C:
          checksum.value = folly::crc32c(data.first, data.second, checksum.value);
          break;
        case ChecksumType::CRC32:
          checksum.value = folly::crc32(data.first, data.second, checksum.value);
          break;
      }
    }

    if (iterBytes != length) {
      XLOGF(WARN, "Iterated bytes {} not equal to length {}, checksum {}", iterBytes, length, checksum);
      return ChecksumInfo{ChecksumType::NONE, 0U};
    }

    return checksum;
  }

  static ChecksumInfo create(ChecksumType type, const uint8_t *buffer, size_t length, uint32_t startingChecksum = ~0U) {
    MemoryDataIterator iter(buffer, length);
    return create(type, &iter, length, startingChecksum);
  }

  Result<Void> combine(const ChecksumInfo &o, size_t length) {
    if (type != ChecksumType::NONE && type != o.type) {
      return makeError(StorageCode::kChecksumMismatch,
                       fmt::format("diffrent type {} != {}", serde::toJsonString(*this), serde::toJsonString(o)));
    }
    if (length == 0) return Void{};
    switch (type) {
      case ChecksumType::NONE:
        *this = o;
        return Void{};

      case ChecksumType::CRC32C:
        value = folly::crc32c_combine(~value, o.value, length);
        return Void{};

      case ChecksumType::CRC32:
        value = folly::crc32_combine(~value, o.value, length);
        return Void{};
    }
  }

  bool operator==(const ChecksumInfo &) const = default;
};
static_assert(serde::Serializable<ChecksumInfo>);

}  // namespace hf3fs::storage

template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::storage::ChunkId> {
  static std::string_view serdeTo(const storage::ChunkId &chunkId) { return chunkId.data(); }
  static Result<storage::ChunkId> serdeFrom(std::string_view str) { return storage::ChunkId(str); }
  static std::string serdeToReadable(const storage::ChunkId &chunkId) { return chunkId.describe(); };
  static Result<storage::ChunkId> serdeFromReadable(std::string_view s) { return storage::ChunkId::fromString(s); }
};

template <>
struct std::hash<hf3fs::storage::ChunkId> {
  size_t operator()(const hf3fs::storage::ChunkId &chunkId) const { return std::hash<std::string>{}(chunkId.data()); }
};

namespace hf3fs::storage {

struct IOResult {
  SERDE_STRUCT_FIELD(lengthInfo, Result<uint32_t>{makeError(StorageClientCode::kNotInitialized)});
  SERDE_STRUCT_FIELD(commitVer, ChunkVer{});
  SERDE_STRUCT_FIELD(updateVer, ChunkVer{});
  SERDE_STRUCT_FIELD(checksum, ChecksumInfo{});
  SERDE_STRUCT_FIELD(commitChainVer, ChainVer{});

 public:
  IOResult() = default;
  // for IOResult(makeError())
  IOResult(folly::Unexpected<Status> &&status)
      : lengthInfo(std::move(status)) {}
  IOResult(uint32_t statusCode)
      : IOResult(makeError(statusCode)) {}
  IOResult(Result<uint32_t> lengthInfo,
           ChunkVer commitVer,
           ChunkVer updateVer,
           ChecksumInfo checksum = {},
           ChainVer commitChainVer = {})
      : lengthInfo(lengthInfo),
        commitVer(commitVer),
        updateVer(updateVer),
        checksum(checksum),
        commitChainVer(commitChainVer) {}

  bool operator==(const IOResult &other) const = default;

  friend void PrintTo(const IOResult &res, std::ostream *os) { *os << fmt::to_string(res); }
};
static_assert(serde::Serializable<IOResult>);

struct VersionedChainId {
  bool operator==(const VersionedChainId &) const = default;
  SERDE_STRUCT_FIELD(chainId, ChainId{});
  SERDE_STRUCT_FIELD(chainVer, ChainVer{});
};
static_assert(serde::Serializable<VersionedChainId>);

struct GlobalKey {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
  SERDE_STRUCT_FIELD(chunkId, ChunkId{});

  static GlobalKey fromFileOffset(const std::vector<VersionedChainId> &chainIds,
                                  const ChunkId &baseChunkId,
                                  const size_t chunkSize,
                                  const size_t fileOffset);
};
static_assert(serde::Serializable<GlobalKey>);

struct UpdateChannel {
  SERDE_STRUCT_FIELD(id, ChannelId{});
  SERDE_STRUCT_FIELD(seqnum, ChannelSeqNum{});
};
static_assert(serde::Serializable<UpdateChannel>);

struct MessageTag {
  SERDE_STRUCT_FIELD(clientId, ClientId{});
  SERDE_STRUCT_FIELD(requestId, RequestId{});
  SERDE_STRUCT_FIELD(channel, UpdateChannel{});

 public:
  MessageTag() = default;
  MessageTag(ClientId clientId, RequestId requestId, UpdateChannel channel = UpdateChannel{})
      : clientId(clientId),
        requestId(requestId),
        channel(channel) {}
};
static_assert(serde::Serializable<MessageTag>);

struct DebugFlags {
  SERDE_STRUCT_FIELD(injectRandomServerError, false);
  SERDE_STRUCT_FIELD(injectRandomClientError, false);
  SERDE_STRUCT_FIELD(numOfInjectPtsBeforeFail, uint16_t{});

 private:
  bool isFailPoint() {
    bool fail = numOfInjectPtsBeforeFail == 1;
    if (numOfInjectPtsBeforeFail > 0) numOfInjectPtsBeforeFail--;
    return fail;
  }

 public:
  bool faultInjectionEnabled() const { return injectRandomServerError || injectRandomClientError; }
  bool injectServerError() { return injectRandomServerError && isFailPoint(); }
  bool injectClientError() { return injectRandomClientError && isFailPoint(); }
};
static_assert(serde::Serializable<DebugFlags>);

struct ReadIO {
  SERDE_STRUCT_FIELD(offset, uint32_t{});
  SERDE_STRUCT_FIELD(length, uint32_t{});
  SERDE_STRUCT_FIELD(key, GlobalKey{});
  SERDE_STRUCT_FIELD(rdmabuf, net::RDMARemoteBuf{});
};
static_assert(serde::Serializable<ReadIO>);

struct UInt8Vector {
  SERDE_STRUCT_FIELD(data, std::vector<uint8_t>{});

 public:
  std::string serdeToReadable() const { return fmt::format("std::vector<uint8_t>({})", data.size()); }
  static Result<UInt8Vector> serdeFromReadable(const std::string &) { return UInt8Vector{}; }
};
static_assert(serde::Serializable<UInt8Vector>);

struct UpdateIO {
  SERDE_STRUCT_FIELD(offset, uint32_t{});
  SERDE_STRUCT_FIELD(length, uint32_t{});
  SERDE_STRUCT_FIELD(chunkSize, uint32_t{});
  SERDE_STRUCT_FIELD(key, GlobalKey{});
  SERDE_STRUCT_FIELD(rdmabuf, net::RDMARemoteBuf{});
  SERDE_STRUCT_FIELD(updateVer, ChunkVer{});
  SERDE_STRUCT_FIELD(updateType, UpdateType{});
  SERDE_STRUCT_FIELD(checksum, ChecksumInfo{});
  SERDE_STRUCT_FIELD(inlinebuf, UInt8Vector{});

 public:
  bool isWrite() const { return updateType == UpdateType::WRITE; }
  bool isTruncate() const { return updateType == UpdateType::TRUNCATE; }
  bool isRemove() const { return updateType == UpdateType::REMOVE; }
  bool isExtend() const { return updateType == UpdateType::EXTEND; }
  bool isCommit() const { return updateType == UpdateType::COMMIT; }
  bool isWriteTruncateExtend() const { return isWrite() || isTruncate() || isExtend(); }
};
static_assert(serde::Serializable<UpdateIO>);

struct CommitIO {
  SERDE_STRUCT_FIELD(key, GlobalKey{});
  SERDE_STRUCT_FIELD(commitVer, ChunkVer{});
  SERDE_STRUCT_FIELD(isSyncing, false);
  SERDE_STRUCT_FIELD(commitChainVer, ChainVer{});
  SERDE_STRUCT_FIELD(isRemove, false);
  SERDE_STRUCT_FIELD(isForce, false);
};

struct BatchReadReq {
  SERDE_STRUCT_FIELD(payloads, std::vector<ReadIO>{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(checksumType, ChecksumType{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<BatchReadReq>);

struct BatchReadRsp {
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(results, std::vector<IOResult>{});
  SERDE_STRUCT_FIELD(inlinebuf, UInt8Vector{});
};
static_assert(serde::Serializable<BatchReadRsp>);

struct WriteReq {
  SERDE_STRUCT_FIELD(payload, UpdateIO{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<WriteReq>);

struct WriteRsp {
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(result, IOResult{});
};
static_assert(serde::Serializable<WriteRsp>);

struct UpdateOptions {
  SERDE_STRUCT_FIELD(isSyncing, bool{});
  SERDE_STRUCT_FIELD(fromClient, bool{});
  SERDE_STRUCT_FIELD(commitChainVer, ChainVer{});
};

struct UpdateReq {
  SERDE_STRUCT_FIELD(payload, UpdateIO{});
  SERDE_STRUCT_FIELD(options, UpdateOptions{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<UpdateReq>);

struct UpdateRsp {
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(result, IOResult{});
};
static_assert(serde::Serializable<UpdateRsp>);

/* queryLastChunk */

struct ChunkIdRange {
  SERDE_STRUCT_FIELD(begin, ChunkId{});
  SERDE_STRUCT_FIELD(end, ChunkId{});
  SERDE_STRUCT_FIELD(maxNumChunkIdsToProcess, uint32_t{});
};
static_assert(serde::Serializable<ChunkIdRange>);

struct QueryLastChunkOp {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
  SERDE_STRUCT_FIELD(chunkIdRange, ChunkIdRange{});
};
static_assert(serde::Serializable<QueryLastChunkOp>);

struct QueryLastChunkReq {
  SERDE_STRUCT_FIELD(payloads, std::vector<QueryLastChunkOp>{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<QueryLastChunkReq>);

struct QueryLastChunkResult {
  SERDE_STRUCT_FIELD(statusCode, Result<Void>{makeError(StorageClientCode::kNotInitialized)});
  SERDE_STRUCT_FIELD(lastChunkId, ChunkId{});
  SERDE_STRUCT_FIELD(lastChunkLen, uint32_t{});
  SERDE_STRUCT_FIELD(totalChunkLen, uint64_t{});
  SERDE_STRUCT_FIELD(totalNumChunks, uint64_t{});
  SERDE_STRUCT_FIELD(moreChunksInRange, bool{});

 public:
  QueryLastChunkResult() = default;
  QueryLastChunkResult(uint32_t statusCode)
      : QueryLastChunkResult(makeError(statusCode), ChunkId{}, 0, 0, 0, false) {}
  QueryLastChunkResult(const Result<Void> &statusCode,
                       const ChunkId &lastChunkId,
                       uint32_t lastChunkLen,
                       uint64_t totalChunkLen,
                       uint64_t totalNumChunks,
                       bool moreChunksInRange)
      : statusCode(statusCode),
        lastChunkId(lastChunkId),
        lastChunkLen(lastChunkLen),
        totalChunkLen(totalChunkLen),
        totalNumChunks(totalNumChunks),
        moreChunksInRange(moreChunksInRange) {}
};
static_assert(serde::Serializable<QueryLastChunkResult>);

struct QueryLastChunkRsp {
  SERDE_STRUCT_FIELD(results, std::vector<QueryLastChunkResult>{});
};
static_assert(serde::Serializable<QueryLastChunkRsp>);

/* removeChunks */

struct RemoveChunksOp {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
  SERDE_STRUCT_FIELD(chunkIdRange, ChunkIdRange{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
};
static_assert(serde::Serializable<RemoveChunksOp>);

struct RemoveChunksReq {
  SERDE_STRUCT_FIELD(payloads, std::vector<RemoveChunksOp>{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<RemoveChunksReq>);

struct RemoveChunksResult {
  SERDE_STRUCT_FIELD(statusCode, Result<Void>{makeError(StorageClientCode::kNotInitialized)});
  SERDE_STRUCT_FIELD(numChunksRemoved, uint32_t{});
  SERDE_STRUCT_FIELD(moreChunksInRange, bool{});

 public:
  RemoveChunksResult() = default;
  RemoveChunksResult(uint32_t statusCode)
      : RemoveChunksResult(makeError(statusCode), 0, false) {}
  RemoveChunksResult(const Result<Void> &statusCode, uint32_t numChunksRemoved, bool moreChunksInRange)
      : statusCode(statusCode),
        numChunksRemoved(numChunksRemoved),
        moreChunksInRange(moreChunksInRange) {}
};
static_assert(serde::Serializable<RemoveChunksResult>);

struct RemoveChunksRsp {
  SERDE_STRUCT_FIELD(results, std::vector<RemoveChunksResult>{});
};
static_assert(serde::Serializable<RemoveChunksRsp>);

/* truncateChunks */

struct TruncateChunkOp {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
  SERDE_STRUCT_FIELD(chunkId, ChunkId{});
  SERDE_STRUCT_FIELD(chunkLen, uint32_t{});
  SERDE_STRUCT_FIELD(chunkSize, uint32_t{});
  SERDE_STRUCT_FIELD(onlyExtendChunk, bool{});
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(retryCount, uint32_t{});
};
static_assert(serde::Serializable<TruncateChunkOp>);

struct TruncateChunksReq {
  SERDE_STRUCT_FIELD(payloads, std::vector<TruncateChunkOp>{});
  SERDE_STRUCT_FIELD(userInfo, flat::UserInfo{});
  SERDE_STRUCT_FIELD(featureFlags, uint32_t{});
  SERDE_STRUCT_FIELD(debugFlags, DebugFlags{});
};
static_assert(serde::Serializable<TruncateChunksReq>);

struct TruncateChunksRsp {
  SERDE_STRUCT_FIELD(results, std::vector<IOResult>{});
};
static_assert(serde::Serializable<TruncateChunksRsp>);

/* GetAllChunkMetadata */

struct ChunkMeta {
  SERDE_STRUCT_FIELD(chunkId, ChunkId{});
  SERDE_STRUCT_FIELD(updateVer, ChunkVer{});
  SERDE_STRUCT_FIELD(commitVer, ChunkVer{});
  SERDE_STRUCT_FIELD(chainVer, ChainVer{});
  SERDE_STRUCT_FIELD(chunkState, ChunkState{});
  SERDE_STRUCT_FIELD(checksum, ChecksumInfo{});
  SERDE_STRUCT_FIELD(length, uint32_t{});
};
static_assert(serde::Serializable<ChunkMeta>);

using ChunkMetaVector = std::vector<ChunkMeta>;

struct GetAllChunkMetadataReq {
  SERDE_STRUCT_FIELD(targetId, TargetId{});
};
static_assert(serde::Serializable<GetAllChunkMetadataReq>);

struct GetAllChunkMetadataRsp {
  SERDE_STRUCT_FIELD(chunkMetaVec, ChunkMetaVector{});
};
static_assert(serde::Serializable<GetAllChunkMetadataRsp>);

struct SyncStartReq {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
};
static_assert(serde::Serializable<SyncStartReq>);

struct TargetSyncInfo {
  SERDE_STRUCT_FIELD(metas, ChunkMetaVector{});
};
static_assert(serde::Serializable<TargetSyncInfo>);

struct SyncDoneReq {
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
};
static_assert(serde::Serializable<SyncDoneReq>);

struct SyncDoneRsp {
  SERDE_STRUCT_FIELD(result, IOResult{});
};
static_assert(serde::Serializable<SyncDoneRsp>);

struct SpaceInfoReq {
  SERDE_STRUCT_FIELD(tag, MessageTag{});
  SERDE_STRUCT_FIELD(force, bool{});
};
static_assert(serde::Serializable<SpaceInfoReq>);

struct SpaceInfo {
  SERDE_STRUCT_FIELD(path, std::string{});
  SERDE_STRUCT_FIELD(capacity, uint64_t{});
  SERDE_STRUCT_FIELD(free, uint64_t{});
  SERDE_STRUCT_FIELD(available, uint64_t{});
  SERDE_STRUCT_FIELD(targetIds, std::vector<hf3fs::flat::TargetId>{});
  SERDE_STRUCT_FIELD(manufacturer, std::string{});
};
static_assert(serde::Serializable<SpaceInfo>);

struct SpaceInfoRsp {
  SERDE_STRUCT_FIELD(spaceInfos, std::vector<SpaceInfo>{});
};
static_assert(serde::Serializable<SpaceInfoRsp>);

struct CreateTargetReq {
  SERDE_STRUCT_FIELD(targetId, TargetId{});
  SERDE_STRUCT_FIELD(diskIndex, uint32_t{});
  SERDE_STRUCT_FIELD(physicalFileCount, 256u);
  SERDE_STRUCT_FIELD(chunkSizeList, (std::vector<Size>{512_KB, 1_MB, 2_MB, 4_MB, 16_MB, 64_MB}));
  SERDE_STRUCT_FIELD(allowExistingTarget, true);
  SERDE_STRUCT_FIELD(chainId, ChainId{});
  SERDE_STRUCT_FIELD(addChunkSize, false);
  SERDE_STRUCT_FIELD(onlyChunkEngine, false);
};

struct CreateTargetRsp {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct OfflineTargetReq {
  SERDE_STRUCT_FIELD(targetId, TargetId{});
  SERDE_STRUCT_FIELD(force, false);
};

struct OfflineTargetRsp {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct RemoveTargetReq {
  SERDE_STRUCT_FIELD(targetId, TargetId{});
  SERDE_STRUCT_FIELD(force, false);
};

struct RemoveTargetRsp {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct QueryChunkReq {
  SERDE_STRUCT_FIELD(chainId, ChainId{});
  SERDE_STRUCT_FIELD(chunkId, ChunkId{});
};

struct ChunkFileId {
  bool operator==(const ChunkFileId &o) const = default;
  SERDE_STRUCT_FIELD(chunkSize, uint32_t{}, nullptr);
  SERDE_STRUCT_FIELD(chunkIdx, uint32_t{}, nullptr);
};

enum class RecycleState : uint8_t {
  NORMAL,
  REMOVAL_IN_PROGRESS,
  REMOVAL_IN_RETRYING,
};

// Metadata of chunk. The order of members has been adjusted for smaller size.
struct ChunkMetadata {
  bool operator==(const ChunkMetadata &o) const = default;

  SERDE_STRUCT_FIELD(commitVer, ChunkVer{}, nullptr);
  SERDE_STRUCT_FIELD(updateVer, ChunkVer{}, nullptr);
  SERDE_STRUCT_FIELD(chainVer, ChainVer{}, nullptr);

  SERDE_STRUCT_FIELD(size, uint32_t{}, nullptr);
  SERDE_STRUCT_FIELD(chunkState, ChunkState{}, nullptr);
  SERDE_STRUCT_FIELD(recycleState, RecycleState::NORMAL, nullptr);
  SERDE_STRUCT_FIELD(checksumType, ChecksumType::NONE, nullptr);
  SERDE_STRUCT_FIELD(checksumValue, uint32_t{}, 0);

  SERDE_STRUCT_FIELD(lastNodeId, uint16_t{}, nullptr);
  SERDE_STRUCT_FIELD(lastRequestId, RequestId{}, nullptr);

  SERDE_STRUCT_FIELD(innerOffset, 0_B, nullptr);
  SERDE_STRUCT_FIELD(innerFileId, ChunkFileId{}, nullptr);

  SERDE_STRUCT_FIELD(lastClientUuid, Uuid{});
  SERDE_STRUCT_FIELD(timestamp, UtcTime{});

 public:
  bool readyToRemove() const { return recycleState != RecycleState::NORMAL; }
  ChecksumInfo checksum() const { return ChecksumInfo{checksumType, checksumValue}; }
};

struct Successor {
  SERDE_STRUCT_FIELD(nodeInfo, flat::NodeInfo{});
  SERDE_STRUCT_FIELD(targetInfo, flat::TargetInfo{});
};

class StorageTarget;
struct Target {
  std::shared_ptr<StorageTarget> storageTarget;
  std::weak_ptr<bool> weakStorageTarget;
  SERDE_STRUCT_FIELD(targetId, TargetId{});
  SERDE_STRUCT_FIELD(path, Path{});
  SERDE_STRUCT_FIELD(diskError, false);
  SERDE_STRUCT_FIELD(lowSpace, false);
  SERDE_STRUCT_FIELD(rejectCreateChunk, false);
  SERDE_STRUCT_FIELD(isHead, false);
  SERDE_STRUCT_FIELD(isTail, false);
  SERDE_STRUCT_FIELD(vChainId, VersionedChainId{});
  SERDE_STRUCT_FIELD(localState, flat::LocalTargetState::INVALID);
  SERDE_STRUCT_FIELD(publicState, flat::PublicTargetState::INVALID);
  SERDE_STRUCT_FIELD(successor, std::optional<Successor>{});
  SERDE_STRUCT_FIELD(diskIndex, uint32_t{});
  SERDE_STRUCT_FIELD(chainId, ChainId{});
  SERDE_STRUCT_FIELD(offlineUponUserRequest, false);
  SERDE_STRUCT_FIELD(useChunkEngine, false);

 public:
  Result<net::Address> getSuccessorAddr() const;

  bool upToDate() const { return localState == flat::LocalTargetState::UPTODATE; }

  bool unrecoverableOffline() const { return diskError || offlineUponUserRequest; }
};
using TargetPtr = std::shared_ptr<const Target>;

struct QueryChunkRsp {
  SERDE_STRUCT_FIELD(target, Target{});
  SERDE_STRUCT_FIELD(meta, Result<ChunkMetadata>{makeError(StatusCode::kInvalidArg)});
};

struct ServiceRequestContext {
  const std::string_view requestType = "dummyReq";
  const MessageTag tag{};
  const uint32_t retryCount{};
  const flat::UserInfo userInfo{};
  DebugFlags debugFlags{};
};

struct StorageEventTrace {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, NodeId{});
  SERDE_STRUCT_FIELD(targetId, TargetId{});
  SERDE_STRUCT_FIELD(updateReq, UpdateReq{});
  SERDE_STRUCT_FIELD(updateRes, IOResult{});
  SERDE_STRUCT_FIELD(forwardRes, IOResult{});
  SERDE_STRUCT_FIELD(commitIO, CommitIO{});
  SERDE_STRUCT_FIELD(commitRes, IOResult{});
};

}  // namespace hf3fs::storage

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::storage::ChunkId> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::ChunkId &chunkId, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "ChunkId({})", chunkId.describe());
  }
};

template <>
struct formatter<hf3fs::storage::ChunkIdRange> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::ChunkIdRange &range, FormatContext &ctx) const {
    if (range.maxNumChunkIdsToProcess == 1) {
      return fmt::format_to(ctx.out(), "{}", range.begin);
    } else {
      return fmt::format_to(ctx.out(),
                            "ChunkIdRange[{}, {}){{{}}}",
                            range.begin.describe(),
                            range.end.describe(),
                            range.maxNumChunkIdsToProcess);
    }
  }
};

template <>
struct formatter<hf3fs::storage::ChecksumInfo> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::ChecksumInfo &checksum, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}#{:08X}", magic_enum::enum_name(checksum.type), ~checksum.value);
  }
};

template <>
struct formatter<hf3fs::storage::IOResult> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::IOResult &result, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(),
                          "length{{{}}} version{{{}/{}}} checksum{{{}}} {{{}}}",
                          result.lengthInfo,
                          result.updateVer,
                          result.commitVer,
                          result.checksum,
                          result.commitChainVer);
  }
};

template <>
struct formatter<hf3fs::storage::UpdateChannel> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::UpdateChannel &channel, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "@{}#{}", channel.id, channel.seqnum);
  }
};

template <>
struct formatter<hf3fs::storage::MessageTag> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::MessageTag &tag, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "@{}#{}:{}", tag.clientId, tag.requestId, tag.channel);
  }
};

FMT_END_NAMESPACE
