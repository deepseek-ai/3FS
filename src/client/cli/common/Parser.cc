#include "Parser.h"

#include <cctype>

namespace hf3fs::client::cli {
namespace {
std::string_view drainSpaces(std::string_view s) {
  auto it = std::find_if(s.begin(), s.end(), [](char c) { return !std::isspace(c); });
  return {it, s.end()};
}
}  // namespace
Result<std::string> ArgsParser::parseOne() {
  s_ = drainSpaces(s_);

  if (s_.empty()) {
    return std::string{};
  }

  std::string res;
  char prevQuoted = '\0';
  auto start = s_.begin();
  bool escaped = false;
  for (;;) {
    auto c = *start;
    if (prevQuoted) {
      if (c == prevQuoted) {
        prevQuoted = '\0';
      } else {
        res.push_back(c);
      }
    } else if (escaped) {
      escaped = false;
      res.push_back(c);
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '\'' || c == '"') {
      prevQuoted = c;
    } else if (std::isspace(c)) {
      break;
    } else {
      res.push_back(c);
    }
    if (++start == s_.end()) {
      break;
    }
  }
  if (prevQuoted || escaped) {
    return makeError(StatusCode::kInvalidArg, fmt::format("leave with {}", prevQuoted ? "quoted" : "escaped"));
  }
  s_ = {start, s_.end()};
  return std::move(res);
}

Result<std::vector<std::string>> ArgsParser::parseAll() {
  std::vector<std::string> v;
  while (!s_.empty()) {
    auto res = parseOne();
    RETURN_ON_ERROR(res);
    if (res->empty()) {
      break;
    }
    v.push_back(res.value());
  }
  return std::move(v);
}
}  // namespace hf3fs::client::cli
