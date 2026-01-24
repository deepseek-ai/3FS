#pragma once

#include <bit>
#include <cstring>
#include <fmt/core.h>
#include <folly/IPAddressV4.h>
#include <folly/SocketAddress.h>
#include <folly/hash/Hash.h>
#include <scn/scn.h>

#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"

namespace hf3fs::net {

constexpr const char *kUnixDomainSocketPrefix[] = {nullptr, "/tmp/domain_socket."};

struct Address {
  uint32_t ip{}; // Stored in Network Byte Order
  uint16_t port{};
  enum Type : uint16_t { TCP, RDMA, IPoIB, LOCAL, UNIX };
  Type type = Type::TCP;
  using is_serde_copyable = void;

  explicit Address(uint64_t addr = 0) { *this = std::bit_cast<Address>(addr); }
  Address(uint32_t ip, uint16_t port, Type type)
      : ip(ip),
        port(port),
        type(type) {}

  bool isTCP() const { return type == Type::TCP || type == Type::IPoIB || type == Type::LOCAL || type == Type::UNIX; }
  bool isRDMA() const { return type == Type::RDMA; }
  bool isUNIX() const { return type == Type::UNIX; }

  bool operator==(const Address &other) const { return uint64_t(*this) == uint64_t(other); }

  Address tcp() const { return Address{ip, port, Type::TCP}; }

  operator uint64_t() const { return std::bit_cast<uint64_t>(*this); }
  std::string str() const {
    auto arr = std::bit_cast<std::array<uint8_t, 4>>(ip);
    return fmt::format("{}://{}.{}.{}.{}:{}", magic_enum::enum_name(type), arr[0], arr[1], arr[2], arr[3], port);
  }
  std::string toString() const { return str(); }
  std::string serdeToReadable() const { return toString(); }
  std::string ipStr() const {
    auto arr = std::bit_cast<std::array<uint8_t, 4>>(ip);
    return fmt::format("{}.{}.{}.{}", arr[0], arr[1], arr[2], arr[3]);
  }

  Result<std::string> domainSocketPath() const {
    if (UNLIKELY(type != Type::UNIX)) {
      return makeError(StatusCode::kInvalidFormat, "invalid type: {} != UNIX", type);
    }
    if (UNLIKELY(ip == 0 || ip >= sizeof(kUnixDomainSocketPrefix) / sizeof(kUnixDomainSocketPrefix[0]))) {
      return makeError(StatusCode::kInvalidFormat, "invalid ip: {}", ip);
    }
    return fmt::format("{}{}", kUnixDomainSocketPrefix[ip], port);
  }

  folly::IPAddressV4 toFollyIP() const { return folly::IPAddressV4::fromLong(ip); }
  folly::SocketAddress toFollyAddress() const {
    if (UNLIKELY(type == UNIX)) {
      auto path = domainSocketPath();
      return path ? folly::SocketAddress::makeFromPath(*path) : folly::SocketAddress{};
    }
    return folly::SocketAddress{toFollyIP().toAddr(), port};
  }
  static Address fromFollyAddress(const folly::SocketAddress &follyAddr, Type type) {
    auto IPAddr = follyAddr.getIPAddress();
    return Address{IPAddr.isV4() ? IPAddr.asV4().toLong() : 0, follyAddr.getPort(), type};
  }

  static Address fromString(std::string_view sv, Type type) {
    std::array<uint8_t, 4> arr;
    uint16_t port;
    auto r = scn::scan(sv, "{}.{}.{}.{}:{}", arr[0], arr[1], arr[2], arr[3], port);
    return r ? Address{std::bit_cast<uint32_t>(arr), port, type} : Address{};
  }

  static Address fromString(std::string_view sv) {
    constexpr std::string_view delimiter = "://";
    auto pos = sv.find(delimiter);
    if (pos != sv.npos) {
      auto addressType = sv.substr(0, pos);
      auto opt = magic_enum::enum_cast<Type>(addressType, magic_enum::case_insensitive);
      if (opt) {
        return fromString(sv.substr(pos + delimiter.size()), *opt);
      }
    }
    return fromString(sv, Type::TCP);
  }

  static Result<Address> from(std::string_view sv) {
    auto addr = fromString(sv);
    if (UNLIKELY(!addr)) {
      return makeError(StatusCode::kInvalidFormat, "invalid address: {}", sv);
    }
    return addr;
  }

  static Result<Address> serdeFromReadable(std::string_view sv) { return from(sv); }
};
static_assert(sizeof(Address) == sizeof(uint64_t), "sizeof(Address) != sizeof(uint64_t)");

}  // namespace hf3fs::net

template <>
struct std::hash<hf3fs::net::Address> {
  size_t operator()(const hf3fs::net::Address &addr) const { return folly::hash::twang_mix64(addr); }
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::net::Address> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::Address &address, FormatContext &ctx) const {
    return formatter<std::string_view>::format(address.str(), ctx);
  }
};

FMT_END_NAMESPACE
