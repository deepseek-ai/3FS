#include "NodeInfo.h"

namespace hf3fs::flat {
std::vector<net::Address> NodeInfo::extractAddresses(const String &serviceName,
                                                     std::optional<net::Address::Type> addressType) const {
  return flat::extractAddresses(app.serviceGroups, serviceName, addressType);
}

std::map<String, std::vector<net::Address>> NodeInfo::getAllServices() const {
  std::map<String, std::vector<net::Address>> res;
  for (const auto &group : app.serviceGroups) {
    for (const auto &service : group.services) {
      auto &v = res[service];
      v.insert(v.end(), group.endpoints.begin(), group.endpoints.end());
    }
  }
  return res;
}
}  // namespace hf3fs::flat
