#pragma once

#include <boost/filesystem.hpp>
#include <filesystem>
#include <fmt/format.h>

namespace hf3fs {

using Path = boost::filesystem::path;

}  // namespace hf3fs

template <>
struct std::hash<hf3fs::Path> {
  size_t operator()(const hf3fs::Path &path) const;
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Path> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::Path &path, FormatContext &ctx) const {
    return formatter<std::string_view>::format(path.string(), ctx);
  }
};

FMT_END_NAMESPACE
