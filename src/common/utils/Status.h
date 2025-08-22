#pragma once

#include <any>
#include <common/utils/StatusCode.h>
#include <common/utils/String.h>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string_view>

#include "folly/Portability.h"

namespace hf3fs {

#if !FOLLY_X64 && !FOLLY_AARCH64
#error "The platform must be 64bit!"
#endif
static_assert(std::endian::native == std::endian::little);

// `Status` imitates `abseil::Status` which contains:
// - code
// - (optional) message
// - (optional) payload of any type
class [[nodiscard]] Status {
  struct status_ok_t {};

 public:
  Status() = delete;
  explicit Status(status_code_t code)
      : data_(construct(code, nullptr)) {}
  Status(const Status &other) { *this = other; }
  Status(Status &&other) = default;

  constexpr static status_ok_t OK{};
  /* implicit */ Status(status_ok_t)
      : Status(StatusCode::kOK) {}

  Status(status_code_t code, std::string_view msg) {
    auto rep = std::make_unique<StatusRep>();
    rep->message = msg;
    data_ = construct(code, std::move(rep));
  }

  Status(status_code_t code, std::string &&msg) {
    auto rep = std::make_unique<StatusRep>();
    rep->message = std::move(msg);
    data_ = construct(code, std::move(rep));
  }

  Status(status_code_t code, const char *msg)
      : Status(code, std::string_view(msg)) {}

  template <typename T>
  Status(status_code_t code, std::string_view msg, T &&payload)
      : Status(code, msg) {
    setPayload(std::forward<T>(payload));
  }

  Status &operator=(const Status &other) {
    if (std::addressof(other) != this) {
      data_ = construct(other.code(), other.rep() ? std::make_unique<StatusRep>(*other.rep()) : nullptr);
    }
    return *this;
  }
  Status &operator=(Status &&other) = default;

  status_code_t code() const { return reinterpret_cast<uintptr_t>(data_.get()) >> kPtrBits; }
  std::string_view message() const { return rep() ? std::string_view(rep()->message) : std::string_view(); }

  Status convert(status_code_t code) const {
    Status status = Status::OK;
    status.data_ = construct(code, rep() ? std::make_unique<StatusRep>(*rep()) : nullptr);
    return status;
  }

  String describe() const {
    return rep() ? fmt::format("{}({}) {}", StatusCode::toString(code()), code(), rep()->message)
                 : fmt::format("{}({})", StatusCode::toString(code()), code());
  }
  std::ostream &operator<<(std::ostream &os) const { return os << describe(); }

  bool isOK() const { return code() == StatusCode::kOK; }
  explicit operator bool() const { return isOK(); }

  bool hasPayload() const { return rep() && rep()->payload.has_value(); }

  template <typename T>
  T *payload() {
    return std::any_cast<T>(&rep()->payload);
  }

  template <typename T>
  const T *payload() const {
    return std::any_cast<const T>(&rep()->payload);
  }

  template <typename T>
  void setPayload(T &&payload) {
    ensuredRep()->payload = std::forward<T>(payload);
  }

  template <typename T, typename... Args>
  void emplacePayload(Args &&...args) {
    ensuredRep()->payload.emplace<T>(std::forward<Args>(args)...);
  }

  void resetPayload() { rep() ? rep()->payload.reset() : void(); }

 private:
  static_assert(StatusCode::kOK == 0, "StatusCode::kOK must be 0!");
  static_assert(sizeof(status_code_t) == 2, "The width of status_code_t must be 16b");

  static constexpr auto kPtrBits = 48u;
  static constexpr auto kPtrMask = ((1ul << kPtrBits) - 1);

  struct StatusRep {
    String message;
    std::any payload;
  };
  struct StatusRepDeleter {
    void operator()(StatusRep *rep) { delete extractPtr(rep); }
  };
  using StatusPtr = std::unique_ptr<StatusRep, StatusRepDeleter>;

  static StatusPtr construct(status_code_t code, std::unique_ptr<StatusRep> rep) {
    return StatusPtr(
        reinterpret_cast<StatusRep *>(reinterpret_cast<uintptr_t>(rep.release()) | (uintptr_t(code) << kPtrBits)));
  }
  static StatusRep *extractPtr(StatusRep *rep) {
    return reinterpret_cast<StatusRep *>(reinterpret_cast<uintptr_t>(rep) & kPtrMask);
  }

  StatusRep *rep() { return extractPtr(data_.get()); }
  const StatusRep *rep() const { return extractPtr(data_.get()); }

  StatusRep *ensuredRep() {
    if (rep() == nullptr) {
      data_ = construct(code(), std::make_unique<StatusRep>());
    }
    return rep();
  }

 private:
  StatusPtr data_;  // |<-- low 48 bits: rep ptr -->|<-- high 16 bits: status code -->|
};

class StatusException : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
  explicit StatusException(Status status)
      : std::runtime_error(status.describe()),
        status_(std::move(status)) {}

  const Status &get() const { return status_; }
  Status &get() { return status_; }

 private:
  Status status_;
};
}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Status> : formatter<hf3fs::status_code_t> {
  template <typename FormatContext>
  auto format(const hf3fs::Status &status, FormatContext &ctx) const {
    auto msg = status.message();
    if (msg.empty()) {
      return fmt::format_to(ctx.out(), "{}({})", hf3fs::StatusCode::toString(status.code()), status.code());
    }
    return fmt::format_to(ctx.out(), "{}({}) {}", hf3fs::StatusCode::toString(status.code()), status.code(), msg);
  }
};

FMT_END_NAMESPACE
