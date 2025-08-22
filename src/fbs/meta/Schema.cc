#include "fbs/meta/Schema.h"

#include <algorithm>
#include <fmt/core.h>
#include <folly/Overload.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <span>
#include <variant>
#include <vector>

#include "common/utils/Result.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/MgmtdTypes.h"

#ifndef TRACK_OFFSET_FOR_CHAIN
// it's best to be a prime number, so each track can start from different chains
#define TRACK_OFFSET_FOR_CHAIN 7
#endif

namespace hf3fs::meta {

Result<Void> Acl::checkPermission(const UserInfo &user, AccessType type) const {
  uint32_t permNeeded = 0;
  if (user.uid == 0)
    permNeeded = 0;
  else if (user.uid == uid)
    permNeeded = type << 6;  // owner
  else if (user.inGroup(gid))
    permNeeded = type << 3;  // group
  else
    permNeeded = type;
  if ((perm & permNeeded) != permNeeded) {
    auto msg = fmt::format("acl {{uid: {}, gid: {}, perm: {:04o}}}, user {} has no perm for {}",
                           uid.toUnderType(),
                           gid.toUnderType(),
                           perm.toUnderType(),
                           user,
                           (int)type);
    XLOGF(DBG, msg);
    return makeError(MetaCode::kNoPermission, std::move(msg));
  }
  return Void{};
}

Result<Void> Acl::checkRecursiveRmPerm(const UserInfo &user, bool owner) const {
  if (iflags & FS_IMMUTABLE_FL) {
    return makeError(MetaCode::kNoPermission, "immutable");
  }
  if (!user.isRoot() && (owner || (perm & S_ISVTX)) && user.uid != uid) {
    return makeError(MetaCode::kNoPermission, "not owner");
  }
  auto rwx = AccessType::READ | AccessType::WRITE | AccessType::EXEC;
  if (!checkPermission(user, rwx)) {
    return makeError(MetaCode::kNoPermission, "no rwx perm");
  }
  return Void{};
}

Result<ChunkId> File::getChunkId(InodeId id, uint64_t offset) const {
  if (!layout.chunkSize) {
    XLOGF(CRITICAL, "File {} chunkSize == 0.", *this);
    return makeError(MetaCode::kInvalidFileLayout, "chunk size = 0");
  }
  auto chunk = offset / layout.chunkSize;
  if (chunk > std::numeric_limits<uint32_t>::max()) {
    XLOGF(CRITICAL, "File {}, offset {}, chunk {} > uint32_max", *this, offset, chunk);
    return MAKE_ERROR_F(MetaCode::kFileTooLarge, "offset {} chunk id {} > uint32_max", offset, chunk);
  }
  return ChunkId(id, 0, chunk);
}

Result<ChainId> File::getChainId(const Inode &inode,
                                 size_t offset,
                                 const flat::RoutingInfo &routingInfo,
                                 uint16_t track) const {
  if ((!layout.chunkSize || !layout.stripeSize)) {
    XLOGF(CRITICAL, "File {} chunkSize {}, stripeSize {}.", inode.id, layout.chunkSize, layout.stripeSize);
    return makeError(MetaCode::kInvalidFileLayout, !layout.chunkSize ? "empty chunksize" : "empty stripesize");
  }
  auto ref = layout.getChainOfChunk(inode, offset / layout.chunkSize + track * TRACK_OFFSET_FOR_CHAIN);
  auto cid = routingInfo.getChainId(ref);
  if (!cid) {
    auto msg = fmt::format("Cannot find ChainId by {}, offset is {}", ref, offset);
    XLOG(ERR, msg);
    return makeError(MgmtdClientCode::kRoutingInfoNotReady, msg);
  }
  return *cid;
}

Layout Layout::newEmpty(ChainTableId table, uint32_t chunk, uint32_t stripe) {
  return {table, ChainTableVersion(0), chunk, stripe, Layout::Empty()};
}

Layout Layout::newEmpty(ChainTableId table, ChainTableVersion tableVer, uint32_t chunk, uint32_t stripe) {
  return {table, tableVer, chunk, stripe, Layout::Empty()};
}

Layout Layout::newChainList(ChainTableId table,
                            ChainTableVersion tableVer,
                            uint32_t chunk,
                            std::vector<uint32_t> chains) {
  return {table, tableVer, chunk, (uint32_t)chains.size(), ChainList{std::move(chains)}};
}

Layout Layout::newChainList(uint32_t chunk, std::vector<flat::ChainId> chains) {
  std::vector<uint32_t> idList;
  idList.reserve(chains.size());
  for (auto c : chains) {
    idList.push_back(c);
  }
  return {ChainTableId(0), ChainTableVersion(0), chunk, (uint32_t)chains.size(), ChainList{std::move(idList)}};
}

Layout Layout::newChainRange(ChainTableId table,
                             ChainTableVersion tableVer,
                             uint32_t chunk,
                             uint32_t stripe,
                             uint32_t baseChainIndex) {
  auto chains = ChainRange(baseChainIndex, ChainRange::Shuffle::STD_SHUFFLE_MT19937, folly::Random::rand64());
  return Layout{table, tableVer, chunk, stripe, chains};
}

Result<Void> Layout::valid(bool allowEmpty) const {
#define INVALID_LAYOUT(msg) makeError(MetaCode::kInvalidFileLayout, msg)
  if (chunkSize == 0) return INVALID_LAYOUT("chunksize is 0");
  if (!folly::isPowTwo(chunkSize.u64())) return INVALID_LAYOUT("chunksize should be power of 2.");
  if (stripeSize == 0) return INVALID_LAYOUT("stripesize is 0");
  auto checkEmptyLayout = [&](const Empty &) -> Result<Void> {
    if (!allowEmpty) return INVALID_LAYOUT("empty layout");
    if (tableId == ChainTableId(0)) return INVALID_LAYOUT("invalid chain table 0");
    return VALID;
  };
  auto checkChainRange = [&](const ChainRange &range) -> Result<Void> {
    if (tableId == 0) return INVALID_LAYOUT("invalid chain table 0");
    if (tableVersion == 0) return INVALID_LAYOUT("invalid chain table version 0 for ChainRange.");
    if (range.baseIndex == 0) return INVALID_LAYOUT("base index is 0");
    return VALID;
  };
  auto checkChainList = [&](const ChainList &list) -> Result<Void> {
    if (tableId == 0 != tableVersion == 0)
      return INVALID_LAYOUT(
          fmt::format("invalid chain table {} version {}", tableId.toUnderType(), tableVersion.toUnderType()));
    if (stripeSize != list.chainIndexes.size())
      return INVALID_LAYOUT(fmt::format("chain list size {} != stripesize {}", list.chainIndexes.size(), stripeSize));
    if (std::any_of(list.chainIndexes.begin(), list.chainIndexes.end(), [](auto v) { return v == 0; }))
      return INVALID_LAYOUT("chain list contains chain index 0");
    return VALID;
  };
  XLOGF_IF(FATAL, chains.valueless_by_exception(), "Chains is valueless");
  return folly::variant_match(chains, checkEmptyLayout, checkChainRange, checkChainList);
#undef INVALID_LAYOUT
}

std::span<const uint32_t> Layout::ChainRange::getChainIndexList(size_t stripe) const {
  const auto &list = chains.try_emplace_with([&]() -> std::vector<uint32_t> {
    std::vector<uint32_t> chains(stripe);
    for (uint32_t i = 0; i < stripe; i++) {
      uint32_t index = baseIndex + i;
      chains[i] = index;
    }
    switch (shuffle) {
      case NO_SHUFFLE:
        break;
      case STD_SHUFFLE_MT19937: {
        auto rng = std::mt19937_64(seed);
        std::shuffle(chains.begin(), chains.end(), rng);
        break;
      }
      default:
        XLOGF(FATAL, "Unknown shuffle func {}", (int)shuffle);
    }
    return chains;
  });
  XLOGF_IF(FATAL, stripe != list.size(), "stripe {} != list {}", stripe, list.size());
  return std::span<const uint32_t>(list);
}

std::span<const uint32_t> Layout::getChainIndexList() const {
  XLOGF_IF(FATAL, chains.valueless_by_exception(), "Chains is valueless");
  return folly::variant_match(
      chains,
      [&](const ChainRange &range) { return range.getChainIndexList(stripeSize); },
      [&](const ChainList &list) { return std::span<const uint32_t>(list.chainIndexes); },
      [&](auto) { return std::span<const uint32_t>(); });
}

ChainRef Layout::getChainOfChunk(const Inode &inode, size_t chunkIndex) const {
  const auto chains = getChainIndexList();
  XLOGF_IF(FATAL, (!stripeSize || chains.empty()), "Inode {}'s layout is invalid", inode);
  auto stripe = chunkIndex % stripeSize;
  return ChainRef{tableId, tableVersion, chains[stripe]};
}

}  // namespace hf3fs::meta
