#pragma once

#include <exception>
#include <unistd.h>
#include <utility>

#include "common/utils/Coroutine.h"

namespace hf3fs {

class FdWrapper {
 public:
  static constexpr int kInvalidFd = -1;

  explicit FdWrapper(int fd = kInvalidFd)
      : fd_(fd) {}
  FdWrapper(const FdWrapper &) = delete;
  FdWrapper(FdWrapper &&o)
      : fd_(o.release()) {}
  ~FdWrapper() { close(); }

  // convert to fd implicit.
  operator int() const { return fd_; }
  bool valid() const { return fd_ != kInvalidFd; }

  // get the ownership of other fd.
  FdWrapper &operator=(const FdWrapper &) = delete;
  FdWrapper &operator=(FdWrapper &&o) {
    close();
    fd_ = o.release();
    return *this;
  }
  FdWrapper &operator=(int fd) {
    close();
    fd_ = fd;
    return *this;
  }

  void close() {
    if (valid()) {
      ::close(fd_);
      fd_ = kInvalidFd;
    }
  }

  // release the ownership of current fd.
  int release() { return std::exchange(fd_, kInvalidFd); }

 private:
  int fd_ = kInvalidFd;
};

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::FdWrapper> : formatter<int> {
  template <typename FormatContext>
  auto format(const hf3fs::FdWrapper &val, FormatContext &ctx) const {
    return formatter<int>::format(static_cast<int>(val), ctx);
  }
};

FMT_END_NAMESPACE
