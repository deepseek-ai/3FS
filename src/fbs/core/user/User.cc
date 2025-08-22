#include "User.h"

#include "common/serde/SerdeComparisons.h"

namespace hf3fs::flat {
UserInfo::UserInfo(Uid uid, Gid gid, std::string token)
    : UserInfo(uid, gid, {}, std::move(token)) {}

UserInfo::UserInfo(Uid uid, Gid gid, std::vector<Gid> groups, std::string token)
    : uid(uid),
      gid(gid),
      groups(std::move(groups)),
      token(std::move(token)) {}

bool UserInfo::isRoot() const { return uid == 0; }

bool UserInfo::inGroup(Gid group) const {
  return gid == group || std::find(groups.begin(), groups.end(), group) != groups.end();
}

bool UserInfo::operator==(const UserInfo &o) const {
  return uid == o.uid && gid == o.gid && groups == o.groups && token == o.token;
}

bool UserInfo::compareUserAndToken(const UserInfo &a, const UserInfo &b) {
  return a.uid == b.uid && a.gid == b.gid && a.token == b.token;
}

bool TokenAttr::active(UtcTime now) const { return isEndless() || endTime > now; }
bool TokenAttr::isEndless() const { return endTime.isZero(); }
bool TokenAttr::operator==(const TokenAttr &other) const { return serde::equals(*this, other); }

Result<Void> UserAttr::validateToken(std::string_view token, UtcTime now) const {
  if (this->token == token) {
    return Void{};
  }
  for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
    const auto &ta = *it;
    if (ta.token == token) {
      if (ta.active(now)) {
        return Void{};
      }
      return makeError(StatusCode::kTokenStale);
    }
  }
  return makeError(StatusCode::kTokenMismatch);
}

Result<Void> UserAttr::addNewToken(std::string_view token,
                                   UtcTime now,
                                   bool invalidateExisted,
                                   size_t maxTokens,
                                   Duration tokenLifetimeExtendLength) {
  if (this->token == token) {
    return Void{};
  }

  if (!invalidateExisted) {
    std::vector<TokenAttr> newTokens;
    for (auto &sa : tokens) {
      if (sa.active(now) && sa.token != token) {
        newTokens.push_back(std::move(sa));
      }
    }
    newTokens.push_back(TokenAttr{this->token, now + tokenLifetimeExtendLength.asUs()});

    if (newTokens.size() + 1 >= maxTokens) {
      return makeError(StatusCode::kTooManyTokens,
                       fmt::format("count of tokens by now: {}. max: {}", newTokens.size(), maxTokens));
    }
    this->tokens = std::move(newTokens);
  }

  this->token = token;
  return Void{};
}

bool UserAttr::operator==(const UserAttr &other) const { return serde::equals(*this, other); }

}  // namespace hf3fs::flat
