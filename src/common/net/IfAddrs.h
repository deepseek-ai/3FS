#pragma once

#include <cerrno>
#include <folly/FBString.h>
#include <folly/IPAddressV4.h>
#include <folly/Range.h>
#include <folly/experimental/Bits.h>
#include <folly/logging/xlog.h>
#include <folly/system/Shell.h>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <sys/ioctl.h>
#include <vector>

#include "common/utils/Result.h"

namespace hf3fs::net {

class IfAddrs {
 public:
  struct Addr {
    folly::IPAddressV4 ip;
    size_t mask;
    bool up;

    Addr(folly::IPAddressV4 ip, size_t mask, bool up)
        : ip(ip),
          mask(mask),
          up(up) {}
    folly::CIDRNetworkV4 subnet() const { return {ip.mask(mask), mask}; }
  };
  using Map = std::multimap<String, Addr>;

  static Result<std::multimap<String, Addr>> load() {
    static Result<std::multimap<String, Addr>> result = loadOnce();
    return result;
  }

 private:
  static Result<Map> loadOnce() {
    Map net2addr;
    struct ifaddrs *ifaddrList;
    if (getifaddrs(&ifaddrList)) {
      XLOGF(ERR, "Failed to call getifaddrs, errno {}", errno);
      return makeError(StatusCode::kOSError, "Failed to call getifaddrs");
    }

    for (auto ifaddrIter = ifaddrList; ifaddrIter != nullptr; ifaddrIter = ifaddrIter->ifa_next) {
      auto name = ifaddrIter->ifa_name;
      if (ifaddrIter->ifa_addr == nullptr || ifaddrIter->ifa_netmask == nullptr) {
        XLOGF(DBG,
              "Skip ifaddr of {}, ifa_addr {} or ifa_netmask {} missing.",
              name,
              (void *)ifaddrIter->ifa_addr,
              (void *)ifaddrIter->ifa_netmask);
        continue;
      }
      if (ifaddrIter->ifa_addr->sa_family != AF_INET || ifaddrIter->ifa_netmask->sa_family != AF_INET) {
        XLOGF(DBG,
              "Skip ifaddr of {}, address family {} {} != AF_INET",
              name,
              ifaddrList->ifa_addr->sa_family,
              ifaddrIter->ifa_netmask->sa_family);
        continue;
      }

      auto ip = folly::IPAddressV4(((sockaddr_in *)ifaddrIter->ifa_addr)->sin_addr);
      auto mask = folly::IPAddressV4(((sockaddr_in *)ifaddrIter->ifa_netmask)->sin_addr).toByteArray();
      auto maskBits = folly::Bits<uint8_t>::count(mask.begin(), mask.end());
      if (folly::IPAddressV4::fetchMask(maskBits) != mask) {
        XLOGF(WARN, "IfAddr of {}, addr {}, mask {} is not valid!!!", name, ip.str(), folly::IPAddressV4(mask).str());
        continue;
      }

      bool up = false;
      int sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock < 0) {
        return makeError(StatusCode::kOSError, "Failed to create socket {}", errno);
      } else {
        struct ifreq ifr;
        strncpy(ifr.ifr_name, ifaddrIter->ifa_name, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
          XLOG(ERR, "Failed to query status of {}, errno {}", name, errno);
          up = false;
        } else {
          up = (ifr.ifr_flags & IFF_UP);
        }
        close(sock);
      }

      auto addr = Addr{ip, maskBits, up};
      net2addr.emplace(name, addr);

      auto fbs = folly::StringPiece(ifaddrIter->ifa_name);
      XLOGF_IF(INFO,
               fbs.startsWith("en") || fbs.startsWith("eth") || fbs.startsWith("ib") || fbs.startsWith("bond"),
               "Get ifaddr of {}, addr {}, subnet {}, up {}",
               ifaddrIter->ifa_name,
               addr,
               addr.subnet(),
               up);
    }

    freeifaddrs(ifaddrList);

    return net2addr;
  }
};

}  // namespace hf3fs::net

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::net::IfAddrs::Addr> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::IfAddrs::Addr &addr, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}/{}", addr.ip.str(), addr.mask);
  }
};

template <>
struct formatter<folly::CIDRNetworkV4> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const folly::CIDRNetworkV4 &network, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}/{}", network.first.str(), network.second);
  }
};

FMT_END_NAMESPACE
