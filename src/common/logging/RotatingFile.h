#pragma once

#include <mutex>

#include "FileHelper.h"
#include "IWritableFile.h"

namespace hf3fs::logging {
class RotatingFile : public IWritableFile {
 public:
  struct Options {
    size_t maxFiles = 0;
    size_t maxFileSize = 0;
    bool rotateOnOpen = false;
  };

  RotatingFile(std::string path, const Options &options);

  bool ttyOutput() const override;
  std::string desc() const override;
  ssize_t writevFull(iovec *iov, int count) override;
  ssize_t writeFull(const void *buf, size_t count) override;
  void flush() override;

 private:
  size_t checkRotate(size_t size);
  void rotate();

  const std::string path_;
  const Options options_;

  mutable std::mutex mu_;
  FileHelper file_;
  size_t currentSize_;
};
}  // namespace hf3fs::logging
