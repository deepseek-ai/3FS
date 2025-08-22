#pragma once

#include <atomic>
#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <fmt/core.h>
#include <folly/Overload.h>
#include <folly/Random.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/DelayedInit.h>
#include <limits>
#include <linux/fs.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/app/ClientId.h"
#include "common/serde/Serde.h"
#include "common/serde/SerdeComparisons.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"
#include "common/utils/UtcTimeSerde.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/ChainTable.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/RoutingInfo.h"

#define SETATTR_TIME_NOW hf3fs::UtcTime::fromMicroseconds(-1)

namespace hf3fs::meta {

using flat::ChainId;
using flat::ChainRef;
using flat::ChainTableId;
using flat::ChainTableVersion;

struct Acl {
  SERDE_STRUCT_FIELD(uid, Uid(0));
  SERDE_STRUCT_FIELD(gid, Gid(0));
  SERDE_STRUCT_FIELD(perm, Permission(0));
  SERDE_STRUCT_FIELD(iflags, IFlags(0));

 public:
  Acl() = default;
  Acl(Uid uid, Gid gid, Permission perm, IFlags iflags = IFlags(0))
      : uid(uid),
        gid(gid),
        perm(perm),
        iflags(iflags) {}

  static Acl root() { return Acl(Uid(0), Gid(0), Permission(0755), IFlags(FS_IMMUTABLE_FL)); }
  static Acl gcRoot() { return Acl(Uid(0), Gid(0), Permission(0700), IFlags(FS_IMMUTABLE_FL)); }

  Result<Void> checkPermission(const UserInfo &user, AccessType type) const;

  Result<Void> checkRecursiveRmPerm(const UserInfo &user, bool owner) const;

  bool operator==(const Acl &o) const { return serde::equals(*this, o); }
};

struct Inode;
struct Layout {
  enum class Type {
    Empty,
    ChainRange,
    ChainList,
  };
  struct Empty {
    static constexpr auto kType = Type::Empty;
    using is_serde_copyable = void;
    std::string serdeToReadable() const { return "empty"; }
    static Result<Empty> serdeFromReadable(std::string_view) { return Empty{}; }
    bool operator==(const Empty &) const { return true; }
  };

  struct ChainRange {
    enum Shuffle : uint8_t { NO_SHUFFLE = 0, STD_SHUFFLE_MT19937 };

    SERDE_STRUCT_FIELD(baseIndex, uint32_t(0));
    SERDE_STRUCT_FIELD(shuffle, Shuffle(0));
    SERDE_STRUCT_FIELD(seed, uint64_t(0));
    mutable folly::DelayedInit<std::vector<uint32_t>> chains;

   public:
    static constexpr auto kType = Type::ChainRange;
    ChainRange() = default;
    ChainRange(uint32_t baseIndex, Shuffle shuffle, uint64_t seed)
        : baseIndex(baseIndex),
          shuffle(shuffle),
          seed(seed),
          chains() {}
    ChainRange(const ChainRange &o)
        : ChainRange(o.baseIndex, o.shuffle, o.seed) {}
    ChainRange &operator=(const ChainRange &o) {
      if (&o != this) {
        new (this) ChainRange(o);
      }
      return *this;
    }

    std::span<const uint32_t> getChainIndexList(size_t stripe) const;
    bool operator==(const ChainRange &o) const { return serde::equals(*this, o); }
  };

  struct ChainList {
    SERDE_STRUCT_FIELD(chainIndexes, std::vector<uint32_t>());

   public:
    static constexpr auto kType = Type::ChainList;
    bool operator==(const ChainList &o) const { return serde::equals(*this, o); }
  };

  // make sure uint64_t is used in file length/offset calculate
  class ChunkSize {
   public:
    using is_serde_copyable = void;
    ChunkSize(uint32_t val = 0)
        : val_(val) {}

    uint32_t u32() const { return val_; }
    uint64_t u64() const { return val_; }
    operator uint64_t() const { return val_; }
    constexpr uint32_t serdeToReadable() const { return val_; }
    static Result<ChunkSize> serdeFromReadable(uint32_t val) { return ChunkSize(val); }

   private:
    uint32_t val_;
  };
  static_assert(sizeof(ChunkSize) == sizeof(uint32_t));

  SERDE_STRUCT_FIELD(tableId, ChainTableId());
  SERDE_STRUCT_FIELD(tableVersion, ChainTableVersion());
  SERDE_STRUCT_FIELD(chunkSize, ChunkSize(0));
  SERDE_STRUCT_FIELD(stripeSize, uint32_t(0));
  SERDE_STRUCT_FIELD(chains, (std::variant<Empty, ChainRange, ChainList>{}));

 public:
  static Layout newEmpty(ChainTableId table, uint32_t chunk, uint32_t stripe);
  static Layout newEmpty(ChainTableId table, ChainTableVersion tableVer, uint32_t chunk, uint32_t stripe);
  static Layout newChainList(ChainTableId table,
                             ChainTableVersion tableVer,
                             uint32_t chunk,
                             std::vector<uint32_t> chains);
  static Layout newChainList(uint32_t chunk, std::vector<flat::ChainId> chains);
  static Layout newChainRange(ChainTableId table,
                              ChainTableVersion tableVer,
                              uint32_t chunk,
                              uint32_t stripe,
                              uint32_t baseChainIndex);

  std::span<const uint32_t> getChainIndexList() const;
  ChainRef getChainOfChunk(const Inode &inode, size_t chunkIndex) const;
  bool empty() const { return std::holds_alternative<Empty>(chains); }
  Result<Void> valid(bool allowEmpty) const;
  Type type() const {
    XLOGF_IF(FATAL, chains.valueless_by_exception(), "Chains is valueless");
    return folly::variant_match(chains, [](const auto &v) { return std::decay_t<decltype(v)>::kType; });
  }
  bool operator==(const Layout &o) const { return serde::equals(*this, o); }
};

enum class InodeType : uint8_t {
  File = 0,
  Directory,
  Symlink,
};

class ChunkId {
  /**
   * Use big endian form to keep order.
   * format: [tenent id (0x00)] + [ unused 0x_00] + [ 64bits inode id ] + [ 16bits track id ] + [ 32bits chunk num ]
   */
  FOLLY_MAYBE_UNUSED std::array<uint8_t, 1> tenent_;
  FOLLY_MAYBE_UNUSED std::array<uint8_t, 1> reserved_;
  std::array<uint8_t, 8> inode_;
  FOLLY_MAYBE_UNUSED std::array<uint8_t, 2> track_;  // reserve for multitrack files.
  std::array<uint8_t, 4> chunk_;

 public:
  // ChunkId(InodeId inode = InodeId(-1), uint32_t chunk = 0)
  //     : ChunkId(inode, 0, chunk) {}
  // ChunkId()
  //     : ChunkId(InodeId(-1), 0, 0) {}
  ChunkId(InodeId inode, uint16_t track, uint32_t chunk)
      : tenent_({0}),
        reserved_({0}),
        inode_(folly::bit_cast<std::array<uint8_t, 8>>(folly::Endian::big64(inode.u64()))),
        track_(folly::bit_cast<std::array<uint8_t, 2>>(folly::Endian::big16(track))),
        chunk_(folly::bit_cast<std::array<uint8_t, 4>>(folly::Endian::big32(chunk))) {}

  std::string pack() const { return std::string((char *)this, sizeof(ChunkId)); }
  static ChunkId unpack(std::string_view data) {
    ChunkId id(InodeId(-1), 0, 0);
    if (data.size() == sizeof(ChunkId)) {
      memcpy(&id, data.data(), sizeof(ChunkId));
    }
    return id;
  }
  static Result<std::pair<ChunkId, ChunkId>> range(InodeId inodeId, uint32_t chunkBegin = 0) {
    if (inodeId.u64() == std::numeric_limits<uint64_t>::max()) {
      return makeError(MetaCode::kNotFile, "InodeId is uint64_max");
    }
    auto begin = ChunkId(inodeId, 0, chunkBegin);
    auto end = ChunkId(inodeId, 1, 0);
    return std::pair<ChunkId, ChunkId>{begin, end};
  }

  InodeId inode() const { return InodeId(folly::Endian::big64(folly::bit_cast<uint64_t>(inode_))); }
  uint16_t track() const { return folly::Endian::big16(folly::bit_cast<uint16_t>(track_)); }
  // use uint64_t to prevent error when calculate file offset or length
  uint64_t chunk() const { return folly::Endian::big32(folly::bit_cast<uint32_t>(chunk_)); }

  operator std::string() const { return pack(); }
  explicit operator bool() const { return *this != ChunkId(InodeId(-1), 0, 0); }
  bool operator==(const ChunkId &o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(ChunkId) == 16);

struct VersionedLength {
  SERDE_STRUCT_FIELD(length, uint64_t(0));
  SERDE_STRUCT_FIELD(truncateVer, uint64_t(0));

 public:
  static std::optional<VersionedLength> mergeHint(std::optional<VersionedLength> h1,
                                                  std::optional<VersionedLength> h2) {
    if (!h1 || !h2) {
      return std::nullopt;
    } else {
      return h1->length >= h2->length ? h1 : h2;
    }
  }

  bool operator==(const VersionedLength &o) const { return serde::equals(*this, o); }
};

struct File {
  struct Flags : BitFlags<Flags, uint32_t> {
    using Base = BitFlags<Flags, uint32_t>;
    using Base::Base;
    static constexpr uint32_t kHasHole = 1;
  };
  SERDE_STRUCT_FIELD(length, uint64_t(0));
  SERDE_STRUCT_FIELD(truncateVer, uint64_t(0));
  SERDE_STRUCT_FIELD(layout, Layout());
  SERDE_STRUCT_FIELD(flags, Flags(0));
  SERDE_STRUCT_FIELD(dynStripe, uint32_t(0));  // dynStripe = 0 means dynamic stripe size is not enabled for this file

 public:
  File() = default;
  File(Layout layout, uint32_t dynStripe = 0)
      : layout(std::move(layout)),
        dynStripe(dynStripe) {}
  static constexpr auto kType = InodeType::File;
  Result<Void> valid() const { return layout.valid(false); }
  bool hasHole() const { return flags.contains(File::Flags::kHasHole); }
  Result<ChunkId> getChunkId(InodeId id, uint64_t offset) const;
  Result<ChainId> getChainId(const Inode &inode,
                             size_t offset,
                             const flat::RoutingInfo &routingInfo,
                             uint16_t track = 0) const;
  VersionedLength getVersionedLength() const { return VersionedLength{length, truncateVer}; }
  void setVersionedLength(VersionedLength v) {
    length = v.length;
    truncateVer = v.truncateVer;
  }
  bool operator==(const File &o) const { return serde::equals(*this, o); }
};

struct Directory {
  struct Lock {
    SERDE_STRUCT_FIELD(client, ClientId(Uuid::zero(), ""));

   public:
    bool operator==(const Lock &o) const { return serde::equals(*this, o); }
  };

  SERDE_STRUCT_FIELD(parent, InodeId());
  SERDE_STRUCT_FIELD(layout, Layout());
  SERDE_STRUCT_FIELD(name, std::string());
  SERDE_STRUCT_FIELD(chainAllocCounter, uint32_t(-1));
  SERDE_STRUCT_FIELD(lock, std::optional<Lock>());

 public:
  static constexpr auto kType = InodeType::Directory;
  Result<Void> valid() const { return layout.valid(true); }
  Result<Void> checkLock(const ClientId &client) const {
    if (lock && lock->client.uuid != client.uuid) {
      return makeError(MetaCode::kNoLock, fmt::format("locked by {}", lock->client));
    }
    return Void{};
  }
  bool operator==(const Directory &o) const { return serde::equals(*this, o); }
};

struct Symlink {
  SERDE_STRUCT_FIELD(target, Path());

 public:
  static constexpr auto kType = InodeType::Symlink;
  Result<Void> valid() const {
    if (target.empty()) return INVALID("target is empty");
    return VALID;
  }
  bool operator==(const Symlink &o) const { return serde::equals(*this, o); }
};

struct InodeData {
  SERDE_STRUCT_FIELD(type, (std::variant<File, Directory, Symlink>()));
  SERDE_STRUCT_FIELD(acl, Acl());
  SERDE_STRUCT_FIELD(nlink, uint16_t(1));
  // 2023/6/1 as default inode timestamps
  SERDE_STRUCT_FIELD(atime, UtcTime(std::chrono::microseconds(1685548800ull * 1000 * 1000)));
  SERDE_STRUCT_FIELD(ctime, UtcTime(std::chrono::microseconds(1685548800ull * 1000 * 1000)));
  SERDE_STRUCT_FIELD(mtime, UtcTime(std::chrono::microseconds(1685548800ull * 1000 * 1000)));

 public:
  InodeType getType() const {
    XLOGF_IF(FATAL, type.valueless_by_exception(), "InodeType is valueless");
    return folly::variant_match(type, [](const auto &v) { return std::decay_t<decltype(v)>::kType; });
  }

#define INODE_TYPE_FUNC(type_name)                                               \
  bool is##type_name() const { return std::holds_alternative<type_name>(type); } \
  type_name &as##type_name() {                                                   \
    XLOGF_IF(FATAL, type.valueless_by_exception(), "InodeType is valueless");    \
    XLOGF_IF(FATAL,                                                              \
             !std::holds_alternative<type_name>(type),                           \
             "Inode type {} != " #type_name,                                     \
             magic_enum::enum_name(getType()));                                  \
    return std::get<type_name>(type);                                            \
  }                                                                              \
  const type_name &as##type_name() const {                                       \
    XLOGF_IF(FATAL, type.valueless_by_exception(), "InodeType is valueless");    \
    XLOGF_IF(FATAL,                                                              \
             !std::holds_alternative<type_name>(type),                           \
             "Inode type {} != " #type_name,                                     \
             magic_enum::enum_name(getType()));                                  \
    return std::get<type_name>(type);                                            \
  }
  INODE_TYPE_FUNC(File)
  INODE_TYPE_FUNC(Directory)
  INODE_TYPE_FUNC(Symlink)
#undef INODE_TYPE_FUNC

  Result<Void> valid() const {
    XLOGF_IF(FATAL, type.valueless_by_exception(), "InodeType is valueless");
    return folly::variant_match(type, [](const auto &v) { return v.valid(); });
  }
  bool operator==(const InodeData &o) const { return serde::equals(*this, o); }
};

struct Inode : InodeData {
  SERDE_STRUCT_FIELD(id, InodeId());

 public:
  Inode() = default;
  explicit Inode(InodeId id)
      : Inode(id, {}) {}
  Inode(InodeId id, InodeData data)
      : InodeData(std::move(data)),
        id(id) {}
  InodeData &data() { return *this; }
  const InodeData &data() const { return *this; }
  Result<Void> valid() const { return data().valid(); }
  bool operator==(const Inode &o) const { return serde::equals(*this, o); }
};

struct GcInfo {
  SERDE_STRUCT_FIELD(user, flat::Uid(0));
  SERDE_STRUCT_FIELD(origPath, Path());

 public:
  bool operator==(const GcInfo &o) const { return serde::equals(*this, o); }
};

struct DirEntryData {
  SERDE_STRUCT_FIELD(id, InodeId());
  SERDE_STRUCT_FIELD(type, InodeType::File);
  SERDE_STRUCT_FIELD(dirAcl, std::optional<Acl>());
  SERDE_STRUCT_FIELD(uuid, Uuid::zero());
  SERDE_STRUCT_FIELD(gcInfo, std::optional<GcInfo>());

 public:
#define INODE_TYPE_FUNC(type_name) \
  bool is##type_name() const { return type == InodeType::type_name; }

  INODE_TYPE_FUNC(File)
  INODE_TYPE_FUNC(Directory)
  INODE_TYPE_FUNC(Symlink)
#undef INODE_TYPE_FUNC

  Result<Void> valid() const {
    if (!magic_enum::enum_contains(type)) return INVALID(fmt::format("invalid type {}", (int)type));
    if ((type == InodeType::Directory) != dirAcl.has_value())
      return INVALID(fmt::format("type {}, dirAcl {}", (type == InodeType::Directory), dirAcl.has_value()));
    return VALID;
  }
  bool operator==(const DirEntryData &o) const { return serde::equals(*this, o); }
};

struct DirEntry : DirEntryData {
  SERDE_STRUCT_FIELD(parent, InodeId());
  SERDE_STRUCT_FIELD(name, std::string());

 public:
  DirEntry() = default;
  DirEntry(InodeId parent, std::string name, DirEntryData data = {})
      : DirEntryData(std::move(data)),
        parent(parent),
        name(std::move(name)) {}
  DirEntryData &data() { return *this; }
  const DirEntryData &data() const { return *this; }
  Result<Void> valid() const { return data().valid(); }
  bool operator==(const DirEntry &o) const { return serde::equals(*this, o); }
};

}  // namespace hf3fs::meta

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::meta::InodeType> {
  static constexpr std::string_view serdeToReadable(hf3fs::meta::InodeType t) { return magic_enum::enum_name(t); }
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::meta::InodeType> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(hf3fs::meta::InodeType type, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}", magic_enum::enum_name(type));
  }
};

template <>
struct formatter<hf3fs::meta::ChunkId> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(hf3fs::meta::ChunkId chunk, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}-{}-{}", chunk.inode(), chunk.track(), chunk.chunk());
  }
};

template <>
struct formatter<hf3fs::meta::Layout::ChunkSize> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(hf3fs::meta::Layout::ChunkSize chunk, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}", (uint64_t)chunk);
  }
};

template <>
struct formatter<hf3fs::meta::VersionedLength> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(hf3fs::meta::VersionedLength v, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}@{}", v.length, v.truncateVer);
  }
};

FMT_END_NAMESPACE
