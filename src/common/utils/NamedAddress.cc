#include "common/utils/NamedAddress.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace hf3fs::net {

template<typename It>
requires std::output_iterator<It, Address>
Result<Void> NamedAddress::resolve(It out) const {
  struct addrinfo req{
    .ai_family = AF_INET,
    .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *res;
  int err = getaddrinfo(node.c_str(), service.c_str(), &req, &res);
  if (err != 0) {
    return MAKE_ERROR_F(StatusCode::kInvalidFormat, "failed to resolve {}:{}: {}", node, service, gai_strerror(err));
  }
  SCOPE_EXIT { freeaddrinfo(res); };

  auto iter = res;
  while (iter != nullptr) {
    if (iter->ai_family == AF_INET) {
      auto sin = (struct sockaddr_in *)iter->ai_addr;
      if (sin->sin_family == AF_INET) {
        *out++ = Address(sin->sin_addr.s_addr,  ntohs(sin->sin_port), type);
      }
    }
    iter = iter->ai_next;
  }
  return Void{};
}

template Result<Void> NamedAddress::resolve(std::back_insert_iterator<std::vector<Address>> out) const;

Result<NamedAddress> NamedAddress::from(std::string_view sv) {
  constexpr std::string_view delimiter = "://";
  auto pos = sv.find(delimiter);
  auto tp = Address::Type::TCP;
  if (pos != sv.npos) {
    auto tpStr = sv.substr(0, pos);
    auto opt = magic_enum::enum_cast<Address::Type>(tpStr, magic_enum::case_insensitive);
    if (!opt) {
      return makeError(StatusCode::kInvalidFormat, "invalid address type: {}", tpStr);
    }
    tp = *opt;
    sv = sv.substr(pos + delimiter.size());
  }
  pos = sv.find_last_of(':');
  if (pos == sv.npos) {
    return makeError(StatusCode::kInvalidFormat, "service not found in address: {}", sv);
  }
  return NamedAddress(std::string(sv.substr(0, pos)), std::string(sv.substr(pos + 1)), tp);
}

NamedAddress to_named(Address addr) {
  return NamedAddress(addr.ipStr(), std::to_string(addr.port), addr.type);
}

std::vector<NamedAddress> to_named(const std::vector<Address> &addrs) {
  std::vector<NamedAddress> named;
  named.reserve(addrs.size());
  for (auto a : addrs) {
    named.push_back(to_named(a));
  }
  return named;
}

}
