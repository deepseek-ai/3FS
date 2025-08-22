#include "BoostFileSystemWrappers.h"

namespace hf3fs {
namespace {
template <typename Func, typename R = std::invoke_result_t<Func, boost::system::error_code &>>
Result<std::conditional_t<std::is_void_v<R>, Void, R>> callBoostFsFunc(Func &&func, std::string_view desc) {
  boost::system::error_code ec;
  if constexpr (std::is_void_v<R>) {
    func(ec);
    if (ec.failed()) {
      return makeError(StatusCode::kIOError, fmt::format("{}: {}", desc, ec.message()));
    }
    return Void{};
  } else {
    auto res = func(ec);
    if (ec.failed()) {
      return makeError(StatusCode::kIOError, fmt::format("{}: {}", desc, ec.message()));
    }
    return res;
  }
}

}  // namespace

Result<bool> boostFilesystemExists(const Path &path) {
  // TODO: how to handle not exists?
  auto res = callBoostFsFunc([&](auto &ec) { return boost::filesystem::exists(path, ec); },
                             fmt::format("check {} exists", path));
  if (res.hasError() && res.error().message().ends_with("No such file or directory")) {
    return false;
  }
  return res;
}

Result<bool> boostFilesystemIsDir(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::is_directory(path, ec); },
                         fmt::format("check {} is directory", path));
}

Result<boost::filesystem::directory_iterator> boostFilesystemDirIter(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::directory_iterator(path, ec); },
                         fmt::format("construct dir iterator of {}", path));
}

Result<bool> boostFilesystemCreateDir(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::create_directory(path, ec); },
                         fmt::format("create dir {}", path));
}

Result<bool> boostFilesystemCreateDirs(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::create_directories(path, ec); },
                         fmt::format("create dir {}", path));
}

Result<bool> boostFilesystemRemove(const Path &path) {
  auto res =
      callBoostFsFunc([&](auto &ec) { return boost::filesystem::remove(path, ec); }, fmt::format("remove {}", path));
  if (res.hasError() && res.error().message().ends_with("No such file or directory")) {
    return true;
  }
  return res;
}

Result<boost::uintmax_t> boostFilesystemRemoveAll(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::remove_all(path, ec); },
                         fmt::format("remove all of {}", path));
}

Result<bool> boostFilesystemIsEmpty(const Path &path) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::is_empty(path, ec); },
                         fmt::format("check {} is directory", path));
}

Result<Void> boostFilesystemCopyFile(const Path &src, const Path &dst) {
  return callBoostFsFunc([&](auto &ec) { boost::filesystem::copy_file(src, dst, ec); },
                         fmt::format("copy file from {} to {}", src, dst));
}

Result<Void> boostFilesystemCreateSymlink(const Path &src, const Path &dst) {
  return callBoostFsFunc([&](auto &ec) { boost::filesystem::create_symlink(src, dst, ec); },
                         fmt::format("create symlink from {} to {}", src, dst));
}

Result<Void> boostFilesystemCreateHardLink(const Path &src, const Path &dst) {
  return callBoostFsFunc([&](auto &ec) { boost::filesystem::create_hard_link(src, dst, ec); },
                         fmt::format("create hard link from {} to {}", src, dst));
}

Result<Void> boostFilesystemRename(const Path &oldPath, const Path &newPath) {
  return callBoostFsFunc([&](auto &ec) { return boost::filesystem::rename(oldPath, newPath, ec); },
                         fmt::format("rename {} to {}", oldPath, newPath));
}
}  // namespace hf3fs
