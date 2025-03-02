#pragma once

#include "common/serde/Serde.h"
#include "common/utils/String.h"
#include "common/utils/SysResource.h"
#include "common/utils/Uuid.h"

namespace hf3fs {

struct ClientId {
  SERDE_STRUCT_TYPED_FIELD(Uuid, uuid, hf3fs::Uuid{});
  SERDE_STRUCT_TYPED_FIELD(String, hostname, "<unknown host>");

 public:
  ClientId()
      : ClientId(Uuid::random()) {}

  explicit ClientId(const Uuid &uuid, std::string_view hostname = "")
      : uuid(uuid),
        hostname(hostname) {
    if (hostname.empty()) {
      auto hostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
      if (hostnameRes) {
        hostname = *hostnameRes;
      }
    }
  }

  static ClientId max() { return ClientId{hf3fs::Uuid::max()}; }

  static ClientId zero() { return ClientId{hf3fs::Uuid::zero()}; }

  static ClientId random(std::string_view hostname = "") { return ClientId{Uuid::random(), hostname}; }

  bool operator==(const ClientId &other) const { return this->uuid == other.uuid; }

  bool operator<(const ClientId &other) const { return this->uuid < other.uuid; }
};
static_assert(serde::Serializable<ClientId>);

}  // namespace hf3fs

template <>
struct std::hash<hf3fs::ClientId> {
  size_t operator()(const hf3fs::ClientId &clientId) const { return std::hash<hf3fs::Uuid>()(clientId.uuid); }
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::ClientId> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::ClientId &clientId, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "ClientId({})@{}", clientId.uuid.toHexString(), clientId.hostname);
  }
};

FMT_END_NAMESPACE
