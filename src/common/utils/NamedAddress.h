#pragma once

#include <string>
#include <iterator>

#include "common/utils/Address.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

struct NamedAddress {
  std::string node, service;
  Address::Type type = Address::Type::TCP;

  NamedAddress(std::string node, std::string service, Address::Type type)
  : node(node), service(service), type(type) {}

  bool operator==(const NamedAddress &other) const { 
    return node == other.node && service == other.service && type == other.type;
  }

  template<typename It>
  requires std::output_iterator<It, Address>
  Result<Void> resolve(It out) const;

  std::string toString() const { 
    return fmt::format("{}://{}:{}", magic_enum::enum_name(type), node, service);
  }

  static Result<NamedAddress> from(std::string_view sv);
};

inline auto format_as(const NamedAddress &a) { return a.toString(); }

net::NamedAddress to_named(net::Address addr);
std::vector<net::NamedAddress> to_named(const std::vector<net::Address> &addrs);

}
