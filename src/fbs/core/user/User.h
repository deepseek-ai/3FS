#pragma once

#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/StrongType.h"
#include "common/utils/Transform.h"
#include "common/utils/UtcTimeSerde.h"

namespace hf3fs::flat {
STRONG_TYPEDEF(uint32_t, Uid);
STRONG_TYPEDEF(uint32_t, Gid);
// todo: add a format func for permission
STRONG_TYPEDEF(uint32_t, Permission);

struct Token : public std::string {
  using std::string::string;
  using std::string::operator=;
  Token() = default;
  Token(const std::string &o)
      : std::string(o) {}
  Token(std::string &&o)
      : std::string(std::move(o)) {}
};

struct UserInfo {
  SERDE_STRUCT_FIELD(uid, Uid());
  SERDE_STRUCT_FIELD(gid, Gid());
  SERDE_STRUCT_FIELD(groups, std::vector<Gid>());
  SERDE_STRUCT_FIELD(token, Token{});

 public:
  UserInfo(Uid uid, Gid gid, std::string token);
  UserInfo(Uid uid = {}, Gid gid = {}, std::vector<Gid> groups = {}, std::string token = {});

  bool isRoot() const;
  bool inGroup(Gid group) const;
  bool operator==(const UserInfo &o) const;
  static bool compareUserAndToken(const UserInfo &a, const UserInfo &b);
};

struct TokenAttr {
  SERDE_STRUCT_FIELD(token, Token{});
  SERDE_STRUCT_FIELD(endTime, UtcTime());  // 0 means endless
 public:
  bool active(UtcTime now) const;
  bool isEndless() const;

  bool operator==(const TokenAttr &other) const;
};

struct UserAttr {
  SERDE_STRUCT_FIELD(name, std::string());
  SERDE_STRUCT_FIELD(token, Token{});  // the only one endless token
  SERDE_STRUCT_FIELD(gid, Gid());
  SERDE_STRUCT_FIELD(groups, std::vector<Gid>());
  SERDE_STRUCT_FIELD(root, false);
  SERDE_STRUCT_FIELD(tokens, std::vector<TokenAttr>());  // tokens with ttl
  SERDE_STRUCT_FIELD(uid, Uid());
  SERDE_STRUCT_FIELD(admin, false);

 public:
  Result<Void> validateToken(std::string_view token, UtcTime now) const;

  Result<Void> addNewToken(std::string_view token,
                           UtcTime now,
                           bool invalidateExisted = true,
                           size_t maxTokens = 5,
                           Duration tokenLifetimeExtendLength = Duration(std::chrono::days(7)));

  bool operator==(const UserAttr &other) const;
};

}  // namespace hf3fs::flat

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::flat::UserInfo> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::flat::UserInfo &user, FormatContext &ctx) const {
    auto groups = transformTo<std::vector>(std::span{user.groups.begin(), user.groups.size()},
                                           [](hf3fs::flat::Gid gid) { return gid.toUnderType(); });
    return fmt::format_to(ctx.out(),
                          "(uid {}, gid {}, groups ({}))",
                          user.uid.toUnderType(),
                          user.gid.toUnderType(),
                          fmt::join(groups.begin(), groups.end(), ","));
  }
};

FMT_END_NAMESPACE

template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::flat::Token> {
  static auto serdeTo(const ::hf3fs::flat::Token &o) { return std::string_view{o}; }
  static Result<::hf3fs::flat::Token> serdeFrom(std::string_view in) { return ::hf3fs::flat::Token{in}; }
  static constexpr std::string_view serdeToReadable(const ::hf3fs::flat::Token &) { return "SECRET TOKEN"; };
};

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::flat::UserInfo> {
  static std::string serdeToReadable(hf3fs::flat::UserInfo user) { return fmt::format("{}", user); }
};
