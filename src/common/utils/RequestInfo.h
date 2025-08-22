#pragma once

#include <folly/io/async/Request.h>
namespace hf3fs {

class RequestInfo : public folly::RequestData {
 public:
  static constexpr const char *kTokenName = "hf3fs::RequestInfo";
  static folly::RequestToken const &token() {
    static folly::RequestToken const token(kTokenName);
    return token;
  }

  static inline RequestInfo *get() {
    return dynamic_cast<RequestInfo *>(folly::RequestContext::get()->getContextData(token()));
  }

  bool hasCallback() override { return false; }

  virtual std::string describe() const = 0;
  virtual bool canceled() const = 0;
};

}  // namespace hf3fs