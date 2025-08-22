#pragma once

#include <array>
#include <boost/detail/bitmask.hpp>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <scn/scn.h>
#include <scn/tuple_return.h>
#include <string_view>
#include <utility>
#include <variant>

#include "common/app/ClientId.h"
#include "common/serde/Serde.h"
#include "common/serde/SerdeComparisons.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"
#include "common/utils/UtcTime.h"
#include "common/utils/UtcTimeSerde.h"
#include "fbs/core/user/User.h"

#define VALID Void()
#define INVALID(str) makeError(StatusCode::kInvalidArg, str)

#define FS_CHAIN_ALLOCATION_FL FS_INDEX_FL /* reuse attr 'I', this directory has it't own chain allocation counter */
#define FS_NEW_CHUNK_ENGINE FS_SECRM_FL    /* reuse attr 's', files under this directory will use new chunk engine */
#define FS_FL_SUPPORTED (FS_IMMUTABLE_FL | FS_CHAIN_ALLOCATION_FL | FS_NEW_CHUNK_ENGINE | FS_HUGE_FILE_FL)
#define FS_FL_INHERITABLE (FS_CHAIN_ALLOCATION_FL | FS_NEW_CHUNK_ENGINE)

namespace hf3fs::meta {

using flat::Gid;
using flat::Permission;
using flat::Uid;
using flat::UserAttr;
using flat::UserInfo;

// see: ioctl_iflags
STRONG_TYPEDEF(uint32_t, IFlags);

struct SessionInfo {
  SERDE_STRUCT_FIELD(client, ClientId::zero());
  SERDE_STRUCT_FIELD(session, Uuid::zero());

 public:
  SessionInfo() = default;
  SessionInfo(ClientId client, Uuid session)
      : client(client),
        session(session) {}
  constexpr bool valid() const { return client != ClientId::zero() && session != Uuid::zero(); }
  constexpr operator bool() const { return valid(); }
};

enum AccessType {
  EXEC = 1,
  WRITE = 2,
  READ = 4,
};

BOOST_BITMASK(AccessType)

template <typename C, typename T>
class BitFlags {
 public:
  using Self = C;
  constexpr BitFlags(T val = 0)
      : val_(val) {}
  using is_serde_copyable = void;

  T mask(T m) const { return val_ & m; }
  bool contains(T bits) const { return mask(bits) == bits; }
  void set(T bits) { val_ |= bits; }
  void clear(T bits) { val_ &= (~bits); }

  T toUnderType() const { return val_; }
  operator const T &() const { return val_; }
  operator T &() { return val_; }
  Self operator|(const T other) const { return Self(val_ | other); }

  std::string serdeToReadable() const { return fmt::format("{:x}", val_); }

  static Result<Self> serdeFromReadable(std::string_view str) {
    auto [r, v] = scn::scan_tuple<T>(str, "{:x}");
    if (!r)
      return makeError(StatusCode::kDataCorruption,
                       fmt::format("SCN({}): {}", magic_enum::enum_name(r.error().code()), r.error().msg()));
    return Self(v);
  }

 private:
  T val_;
};

class OpenFlags : public BitFlags<OpenFlags, int32_t> {
 public:
  using Base = BitFlags<OpenFlags, int32_t>;
  using Base::Base;

  AccessType accessType() const {
    switch (mask(O_ACCMODE)) {
      case O_RDONLY:
        return AccessType::READ;
      case O_WRONLY:
        return AccessType::WRITE;
      case O_RDWR:
      default:
        return AccessType::READ | AccessType::WRITE;
    }
  }
  operator AccessType() const { return accessType(); }

  Result<Void> valid() const {
    if (contains(O_DIRECTORY)) {
      if (accessType() != AccessType::READ) return makeError(StatusCode::kInvalidArg, "O_DIRECTORY & WRITE");
      if (contains(O_TRUNC)) return makeError(StatusCode::kInvalidArg, "O_DIRECTORY & O_TRUNC");
    }
    return Void{};
  }
};

class AtFlags : public BitFlags<AtFlags, int32_t> {
 public:
  explicit AtFlags(int32_t val = 0)
      : BitFlags<AtFlags, int32_t>(val) {}

  Result<Void> valid() const { return Void{}; }
  bool followLastSymlink() const { return contains(AT_SYMLINK_FOLLOW) && !contains(AT_SYMLINK_NOFOLLOW); }
};

class InodeId {
 public:
  // Use little endian form as key in FoundationDB, it helps to avoid hot spot
  using Key = std::array<uint8_t, 8>;
  using is_serde_copyable = void;

  static constexpr size_t trees() { return 2; }
  static constexpr InodeId root() { return InodeId(0); }
  static constexpr InodeId gcRoot() { return InodeId(1); }

  // InodeId: 0 ~ 0x01ffffffffffffff
  // If InodeId starts with 0x01: use new 3FS Chunk Engine
  static constexpr uint64_t kNewChunkEngineMask = (1ull << 56);
  static_assert(kNewChunkEngineMask == 0x0100000000000000ull);
  static constexpr InodeId withNewChunkEngine(InodeId orig) { return InodeId(orig.u64() | kNewChunkEngineMask); }
  static constexpr InodeId normalMax() { return InodeId::withNewChunkEngine(InodeId((1ull << 56) - 1)); }

  // special InodeId used by Client
  // 3fs-virt InodeId: 0xfffffffffffffffe
  static constexpr InodeId virt() { return InodeId(uint64_t(-2)); }
  // iov InodeId range [0xffffffff7ffe0002, 0xffffffff7fff0001]
  static constexpr int iovIidStart = 65535;
  static constexpr InodeId iovDir() { return InodeId((uint64_t) - (1ull << 31)); }
  static constexpr InodeId iov(int iovd) { return InodeId(iovDir().u64() - iovIidStart - iovd); }

  // 3fs-virt/rm-rf: 0xfffffffffffffffd
  static constexpr InodeId rmRf() { return InodeId(-3); }
  // temporary InodeId used by rm-rf or mv symlink: [0xfdffffe700000001, 0xffffffe700000000]
  static constexpr InodeId virtTemporary(InodeId toRemove) { return InodeId(-(100ull << 30) - toRemove.u64()); }

  // 0xffffffff00000000
  static constexpr InodeId getConf() { return InodeId(-((uint64_t)2 << 31)); }
  // 0xfffffffe80000000
  static constexpr InodeId setConf() { return InodeId(-((uint64_t)3 << 31)); }

  explicit constexpr InodeId(uint64_t val = 0)
      : val_(val) {}

  // InodeId &operator=(uint64_t val) {
  //   val_ = val;
  //   return *this;
  // }

  Key packKey() const {
    auto le = folly::Endian::little(val_);
    return folly::bit_cast<Key>(le);
  }
  static InodeId unpackKey(const Key &key) {
    auto le = folly::bit_cast<uint64_t>(key);
    return InodeId(folly::Endian::little(le));
  }

  std::string toHexString() const { return fmt::format("0x{:016x}", val_); }
  std::string serdeToReadable() const { return toHexString(); }
  operator folly::dynamic() const { return toHexString(); }
  constexpr uint64_t u64() const { return val_; }
  // constexpr operator uint64_t() const { return u64(); }
  constexpr auto operator<=>(const InodeId &o) const { return this->val_ <=> o.val_; }
  constexpr auto operator==(const InodeId &o) const { return this->val_ == o.val_; }

  bool isTreeRoot() const { return *this == root() || *this == gcRoot(); }

  bool useNewChunkEngine() const { return u64() & kNewChunkEngineMask; }

 private:
  uint64_t val_;
};

// check 3fs-virt InodeIds
static_assert(InodeId::normalMax().u64() == 0x01ffffffffffffffull);
static_assert(InodeId::virt().u64() == 0xfffffffffffffffeull);
static_assert(InodeId::iovDir().u64() == 0xffffffff80000000ull);
static_assert(InodeId::iov(0).u64() == 0xffffffff7fff0001ull);
static_assert(InodeId::iov(65535).u64() == 0xffffffff7ffe0002ull);
static_assert(InodeId::rmRf().u64() == 0xfffffffffffffffdull);
static_assert(InodeId::virtTemporary(InodeId(0)).u64() == 0xffffffe700000000ull);
static_assert(InodeId::virtTemporary(InodeId(0x1ffffffffffffff)).u64() == 0xfdffffe700000001ull);
static_assert(InodeId::getConf().u64() == 0xffffffff00000000ull);
static_assert(InodeId::setConf().u64() == 0xfffffffe80000000ull);

// check 3fs-virt InodeIds won't conflict with each other and normal InodeIds
static constexpr auto spacialInodeRanges =
    std::to_array({InodeId::normalMax(),
                   InodeId(0xfd00000000000000ull),  // all virt must start with 0xf
                   InodeId::virtTemporary(InodeId::normalMax()),
                   InodeId::virtTemporary(InodeId(0)),
                   InodeId::setConf(),
                   InodeId::getConf(),
                   InodeId::iov(65535),
                   InodeId::iov(0),
                   InodeId::iovDir(),
                   InodeId::rmRf(),
                   InodeId::virt()});
constexpr inline bool checkSpecialInode() {
  for (uint64_t i = 1; i < spacialInodeRanges.size(); i++) {
    if (spacialInodeRanges[i] <= spacialInodeRanges[i - 1]) {
      return false;
    }
  }
  return true;
}
static_assert(checkSpecialInode());

struct PathAt {
  SERDE_STRUCT_FIELD(parent, InodeId(0));
  SERDE_STRUCT_FIELD(path, std::optional<Path>());

 public:
  PathAt(InodeId parent = InodeId::root())
      : PathAt(parent, std::nullopt) {}
  PathAt(Path path)
      : PathAt(InodeId::root(), std::move(path)) {}
  PathAt(std::string path)
      : PathAt(Path(path)) {}
  PathAt(const char *p)
      : PathAt(Path(p)) {}
  PathAt(InodeId parent, std::optional<Path> path)
      : parent(parent),
        path(std::move(path)) {}
  Result<Void> validForCreate() const {
    if (!path.has_value() || path->empty()) return INVALID("path not set");
    if (!path->has_filename()) return INVALID("doesn't have filename");
    if (path->filename_is_dot()) return INVALID("filename is .");
    if (path->filename_is_dot_dot()) return INVALID("filename is ..");
    if (path->filename() == "/") return INVALID("filename is /");
    return VALID;
  }
  bool operator==(const PathAt &o) const { return serde::equals(*this, o); }
};

}  // namespace hf3fs::meta

template <>
struct std::hash<hf3fs::meta::InodeId> {
  size_t operator()(const hf3fs::meta::InodeId &id) const { return robin_hood::hash_int(id.u64()); }
};

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::meta::InodeId> {
  static constexpr auto serdeTo(const hf3fs::meta::InodeId &id) { return id.u64(); }
  static Result<hf3fs::meta::InodeId> serdeFrom(uint64_t id) { return hf3fs::meta::InodeId(id); }
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::meta::InodeId> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::meta::InodeId &id, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "0x{:016x}", id.u64());
  }
};

FMT_END_NAMESPACE
