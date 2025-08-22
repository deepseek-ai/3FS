#pragma once

#include <fmt/format.h>
#include <optional>

namespace hf3fs {
template <typename T>
inline std::optional<std::decay_t<T>> makeOptional(const T *ptr) {
  if (ptr) return std::make_optional<T>(*ptr);
  return std::nullopt;
}

template <typename T>
auto optionalMap(const std::optional<T> &src, auto &&transformer) {
  using DstType = std::decay_t<decltype(transformer(src.value()))>;
  if (src.has_value()) {
    return std::optional<DstType>(transformer(src.value()));
  }
  return std::optional<DstType>();
}

template <typename T>
struct OptionalFmt {
  explicit OptionalFmt(const std::optional<T> &val)
      : val_(val) {}

  std::string toString() const { return fmt::format("{}", *this); }

  const std::optional<T> &val_;
};
}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <typename T>
struct formatter<hf3fs::OptionalFmt<T>> : formatter<T> {
  template <typename FormatContext>
  auto format(const hf3fs::OptionalFmt<T> &op, FormatContext &ctx) const {
    if (op.val_.has_value()) {
      return fmt::format_to(ctx.out(), "{}", *op.val_);
    } else {
      return fmt::format_to(ctx.out(), "std::nullopt");
    }
  }
};

FMT_END_NAMESPACE
