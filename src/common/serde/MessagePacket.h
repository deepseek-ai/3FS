#pragma once

#include <type_traits>

#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"

namespace hf3fs::serde {

enum EssentialFlags : uint16_t {
  IsReq = (1 << 0),
  UseCompress = (1 << 1),
  ControlRDMA = (1 << 2),
};

struct Version {
  SERDE_STRUCT_FIELD(major, uint8_t{});
  SERDE_STRUCT_FIELD(minor, uint8_t{});
  SERDE_STRUCT_FIELD(patch, uint8_t{});
  SERDE_STRUCT_FIELD(hash, uint32_t{});
};

template <class T>
struct PointerWrapper {
 public:
  PointerWrapper() = default;
  explicit PointerWrapper(const T &t)
      : t(&t) {}
  const T *t = nullptr;
};

template <class T>
using Payload = std::conditional_t<std::is_same_v<T, Void>, std::string_view, PointerWrapper<T>>;

struct Timestamp {
  auto serverLatency() const { return Duration(std::chrono::microseconds{serverSerialized - serverReceived}); }
  auto inflightLatency() const { return Duration(std::chrono::microseconds{clientReceived - clientSerialized}); }
  auto networkLatency() const { return inflightLatency() - serverLatency(); }
  auto queueLatency() const { return Duration(std::chrono::microseconds{serverWaked - serverReceived}); }
  auto totalLatency() const { return Duration(std::chrono::microseconds{clientWaked - clientCalled}); }

  SERDE_STRUCT_FIELD(clientCalled, int64_t{});
  SERDE_STRUCT_FIELD(clientSerialized, int64_t{});
  SERDE_STRUCT_FIELD(serverReceived, int64_t{});
  SERDE_STRUCT_FIELD(serverWaked, int64_t{});
  SERDE_STRUCT_FIELD(serverProcessed, int64_t{});
  SERDE_STRUCT_FIELD(serverSerialized, int64_t{});
  SERDE_STRUCT_FIELD(clientReceived, int64_t{});
  SERDE_STRUCT_FIELD(clientWaked, int64_t{});
};

template <class T = Void>
struct MessagePacket {
  MessagePacket() requires(std::is_same_v<T, Void>) = default;
  explicit MessagePacket(const T &req)
      : payload(req) {}

  bool isRequest() const { return flags & EssentialFlags::IsReq; }
  bool useCompress() const { return flags & EssentialFlags::UseCompress; }
  bool controlRDMA() const { return flags & EssentialFlags::ControlRDMA; }

  SERDE_STRUCT_FIELD(uuid, uint64_t{});
  SERDE_STRUCT_FIELD(serviceId, uint16_t{});
  SERDE_STRUCT_FIELD(methodId, uint16_t{});
  SERDE_STRUCT_FIELD(flags, uint16_t{});
  SERDE_STRUCT_FIELD(version, Version{});
  SERDE_STRUCT_FIELD(payload, Payload<T>{});
  SERDE_STRUCT_FIELD(timestamp, std::optional<Timestamp>{});
};

}  // namespace hf3fs::serde

template <class T>
requires(!std::is_same_v<T, hf3fs::Void>) struct hf3fs::serde::SerdeMethod<hf3fs::serde::PointerWrapper<T>> {
  static void serialize(const hf3fs::serde::PointerWrapper<T> &payload, auto &out) {
    auto size = out.tableBegin(false);
    serde::serialize(*payload.t, out);
    out.tableEnd(size);
  }

  static void serializeReadable(const hf3fs::serde::PointerWrapper<T> &payload, auto &out) {
    serde::serialize(*payload.t, out);
  }

  static Result<Void> deserialize(hf3fs::serde::PointerWrapper<T> &, auto &) {
    return makeError(StatusCode::kSerdeNotContainer);
  }
};
