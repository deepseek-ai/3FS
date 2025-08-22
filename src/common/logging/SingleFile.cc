#include "SingleFile.h"

#include <fmt/format.h>
#include <folly/FileUtil.h>

namespace hf3fs::logging {
bool SingleFile::ttyOutput() const { return isatty(file_.fd()); }

std::string SingleFile::desc() const { return fmt::format("fd({})", file_.fd()); }

ssize_t SingleFile::writevFull(iovec *iov, int count) { return folly::writevFull(file_.fd(), iov, count); }

ssize_t SingleFile::writeFull(const void *buf, size_t count) { return folly::writeFull(file_.fd(), buf, count); }

void SingleFile::flush() {
  // do nothing since folly::File is unbuffered
}
}  // namespace hf3fs::logging
