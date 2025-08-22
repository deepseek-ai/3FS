#pragma once

#include <folly/File.h>

#include "IWritableFile.h"

namespace hf3fs::logging {
class SingleFile : public IWritableFile {
 public:
  SingleFile(folly::File &&file)
      : file_(std::move(file)) {}

  bool ttyOutput() const override;
  std::string desc() const override;
  ssize_t writevFull(iovec *iov, int count) override;
  ssize_t writeFull(const void *buf, size_t count) override;
  void flush() override;

 private:
  folly::File file_;
};
}  // namespace hf3fs::logging
