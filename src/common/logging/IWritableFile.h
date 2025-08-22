#pragma once

#include <folly/FileUtil.h>
#include <string>

namespace hf3fs::logging {
class IWritableFile {
 public:
  virtual ~IWritableFile() = default;

  virtual bool ttyOutput() const = 0;
  virtual std::string desc() const = 0;
  virtual ssize_t writevFull(iovec *iov, int count) = 0;
  virtual ssize_t writeFull(const void *buf, size_t count) = 0;
  virtual void flush() = 0;
};
}  // namespace hf3fs::logging
