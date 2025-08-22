#pragma once

#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Reflection.h"

namespace hf3fs::net {
struct UserRequestOptions;
}

namespace hf3fs::serde {

template <NameWrapper NAME, uint32_t ID>
struct ServiceBase {
  static constexpr auto kServiceNameWrapper = NAME;
  static constexpr std::string_view kServiceName = NAME;
  static constexpr uint16_t kServiceID = ID;
};

template <NameWrapper NAME, class T, class REQ, class RSP, uint16_t ID, auto METHOD>
struct MethodInfo {
  static constexpr auto nameWrapper = NAME;
  static constexpr std::string_view name = NAME;
  using Object = T;
  using ReqType = REQ;
  using RspType = RSP;
  static constexpr auto id = ID;
  static constexpr auto method = METHOD;
};

template <class T>
constexpr inline uint16_t MaxMethodId = 0;
template <class... T>
constexpr inline uint16_t MaxMethodId<std::tuple<T...>> = std::max({T::id...});

template <class T, template <class> class Service>
struct ServiceWrapper {
  static constexpr std::string_view kServiceName = Service<void>::kServiceName;
  static constexpr uint16_t kServiceID = Service<void>::kServiceID;

 protected:
  friend struct ::hf3fs::refl::Helper;
  template <class U = T>
  static auto CollectField(::hf3fs::refl::Rank<> rank) -> refl::Helper::FieldInfoList<Service<U>>;
};

template <class T, class C, auto DEFAULT = nullptr>
class MethodExtractor {
 public:
  static auto get(uint16_t id) {
    constexpr MethodExtractor ins;
    return id <= ins.kMaxThreadId ? ins.table[id] : DEFAULT;
  }

 protected:
  consteval MethodExtractor() {
    for (uint16_t i = 0; i <= kMaxThreadId; ++i) {
      table[i] = calc(i);
    }
  }

  template <size_t I = 0>
  consteval auto calc(uint16_t id) {
    if constexpr (I == std::tuple_size_v<FieldInfoList>) {
      return DEFAULT;
    } else {
      using FieldInfo = std::tuple_element_t<I, FieldInfoList>;
      return FieldInfo::id == id ? &C::template call<FieldInfo> : calc<I + 1>(id);
    }
  }

 private:
  using FieldInfoList = refl::Helper::FieldInfoList<T>;
  using Method = decltype(&C::template call<std::tuple_element_t<0, FieldInfoList>>);
  static constexpr auto kMaxThreadId = MaxMethodId<FieldInfoList>;
  std::array<Method, kMaxThreadId + 1> table;
};

#define SERDE_SERVICE(NAME, ID) SERDE_SERVICE_2(NAME, NAME, ID)

#define SERDE_SERVICE_2(STRUCT_NAME, SERVICE_NAME, ID) \
  template <class T = void>                            \
  struct STRUCT_NAME : public ::hf3fs::serde::ServiceBase<#SERVICE_NAME, ID>

#define SERDE_SERVICE_METHOD(NAME, ID, REQ, RSP)  \
  SERDE_SERVICE_METHOD_SENDER(NAME, ID, REQ, RSP) \
  SERDE_SERVICE_METHOD_REFL(NAME, ID, REQ, RSP)

#define SERDE_SERVICE_METHOD_SENDER(NAME, ID, REQ, RSP)                                                    \
 public:                                                                                                   \
  static constexpr auto NAME##MethodId = ID;                                                               \
                                                                                                           \
  template <class Context>                                                                                 \
  static CoTryTask<RSP> NAME(Context &ctx,                                                                 \
                             const REQ &req,                                                               \
                             const ::hf3fs::net::UserRequestOptions *options = nullptr,                    \
                             ::hf3fs::serde::Timestamp * timestamp = nullptr) {                            \
    co_return co_await ctx.template call<kServiceNameWrapper, #NAME, REQ, RSP, kServiceID, ID>(req,        \
                                                                                               options,    \
                                                                                               timestamp); \
  }                                                                                                        \
                                                                                                           \
  template <class Context>                                                                                 \
  static Result<RSP> NAME##Sync(Context &ctx,                                                              \
                                const REQ &req,                                                            \
                                const ::hf3fs::net::UserRequestOptions *options = nullptr,                 \
                                ::hf3fs::serde::Timestamp * timestamp = nullptr) {                         \
    return ctx.template callSync<kServiceNameWrapper, #NAME "Sync", REQ, RSP, kServiceID, ID>(req,         \
                                                                                              options,     \
                                                                                              timestamp);  \
  }

#define SERDE_SERVICE_METHOD_REFL(NAME, ID, REQ, RSP)                                                 \
 private:                                                                                             \
  struct MethodId##ID : std::type_identity<REFL_NOW> {};                                              \
  static auto CollectField(::hf3fs::refl::Rank<std::tuple_size_v<typename MethodId##ID::type> + 1>) { \
    if constexpr (std::is_void_v<T>) {                                                                \
      return ::hf3fs::refl::Append_t<typename MethodId##ID::type,                                     \
                                     ::hf3fs::serde::MethodInfo<#NAME, T, REQ, RSP, ID, nullptr>>{};  \
    } else {                                                                                          \
      return ::hf3fs::refl::Append_t<typename MethodId##ID::type,                                     \
                                     ::hf3fs::serde::MethodInfo<#NAME, T, REQ, RSP, ID, &T::NAME>>{}; \
    }                                                                                                 \
  }                                                                                                   \
  friend struct ::hf3fs::refl::Helper

#define SERDE_SERVICE_CLIENT(CLIENT_NAME, BASE_NAME)                                                  \
  template <class T = void>                                                                           \
  struct CLIENT_NAME {                                                                                \
    static constexpr auto kServiceName = BASE_NAME<T>::kServiceName;                                  \
    static constexpr auto kServiceID = BASE_NAME<T>::kServiceID;                                      \
                                                                                                      \
    template <NameWrapper name, typename Context, typename Req>                                       \
    static auto send(Context &ctx,                                                                    \
                     const Req &req,                                                                  \
                     const ::hf3fs::net::UserRequestOptions *options = nullptr,                       \
                     ::hf3fs::serde::Timestamp *timestamp = nullptr) {                                \
      auto handler = [&](auto type) requires(decltype(type)::name == name &&                          \
                                             std::is_same_v<Req, typename decltype(type)::ReqType>) { \
        using M = std::decay_t<decltype(type)>;                                                       \
        return ctx.template call<BASE_NAME<T>::kServiceNameWrapper,                                   \
                                 name,                                                                \
                                 typename M::ReqType,                                                 \
                                 typename M::RspType,                                                 \
                                 BASE_NAME<T>::kServiceID,                                            \
                                 M::id>(req, options, timestamp);                                     \
      };                                                                                              \
      return ::hf3fs::refl::Helper::visit<BASE_NAME<T>>(std::move(handler));                          \
    }                                                                                                 \
  }

}  // namespace hf3fs::serde
