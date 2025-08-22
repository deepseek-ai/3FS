#pragma once

#include <algorithm>
#include <array>
#include <asm-generic/errno-base.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <folly/Likely.h>
#include <folly/Range.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/fibers/Semaphore.h>
#include <folly/futures/detail/Types.h>
#include <folly/io/IOBufQueue.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest_prod.h>
#include <infiniband/verbs.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sys/socket.h>
#include <utility>

#include "common/net/ib/IBDevice.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

using RDMABufMR = ibv_mr *;

class RDMARemoteBuf {
  struct Rkey {
    uint32_t rkey = 0;
    int devId = -1;

    bool operator==(const Rkey &) const = default;
  };

 public:
  RDMARemoteBuf()
      : addr_(0),
        length_(0),
        rkeys_() {}

  RDMARemoteBuf(uint64_t addr, size_t length, std::array<Rkey, IBDevice::kMaxDeviceCnt> rkeys)
      : addr_(addr),
        length_(length),
        rkeys_(rkeys) {}

  ~RDMARemoteBuf() = default;

  uint64_t addr() const { return addr_; }
  size_t size() const { return length_; }

  std::optional<uint32_t> getRkey(int devId) const {
    for (auto &rkey : rkeys_) {
      if (rkey.devId == devId) {
        return rkey.rkey;
      }
    }
    return std::nullopt;
  }

  bool advance(size_t len) {
    if (UNLIKELY(length_ < len)) {
      return false;
    }
    addr_ += len;
    length_ -= len;
    return true;
  }
  bool subtract(size_t n) {
    if (UNLIKELY(n > size())) {
      return false;
    }
    length_ -= n;
    return true;
  }

  RDMARemoteBuf subrange(size_t offset, size_t len) const {
    if (UNLIKELY(offset + len > length_)) {
      return RDMARemoteBuf();
    }
    RDMARemoteBuf remote = *this;
    remote.addr_ = addr_ + offset;
    remote.length_ = len;
    return remote;
  }

  RDMARemoteBuf first(size_t len) const { return subrange(0, len); }
  RDMARemoteBuf takeFirst(size_t len) {
    auto buf = first(len);
    advance(len);
    return buf;
  }

  RDMARemoteBuf last(size_t len) const {
    if (UNLIKELY(len > length_)) {
      return RDMARemoteBuf();
    }
    return subrange(length_ - len, len);
  }
  RDMARemoteBuf takeLast(size_t len) {
    auto buf = last(len);
    subtract(len);
    return buf;
  }

  bool valid() const { return addr_ != 0; }
  explicit operator bool() const { return valid(); }

  bool operator==(const RDMARemoteBuf &o) const = default;

  auto &rkeys() { return rkeys_; }
  const auto &rkeys() const { return rkeys_; }

 private:
  friend class RDMABuf;
  FRIEND_TEST(TestRDMARemoteBuf, Basic);
  FRIEND_TEST(TestIBSocket, RDMAFailure);

  uint64_t addr_;
  uint64_t length_;
  std::array<Rkey, IBDevice::kMaxDeviceCnt> rkeys_;
};

class RDMABufPool;

class RDMABuf {
 public:
  RDMABuf()
      : RDMABuf(nullptr, nullptr, 0) {}

  RDMABuf(const RDMABuf &) = default;
  RDMABuf(RDMABuf &&) = default;

  RDMABuf &operator=(const RDMABuf &) = default;
  RDMABuf &operator=(RDMABuf &&) = default;

  static RDMABuf allocate(size_t size) { return allocate(size, {}); }

  bool valid() const { return buf_ != nullptr; }
  explicit operator bool() const { return valid(); }
  auto raw() const { return buf_.get(); }

  const uint8_t *ptr() const { return begin_; }
  uint8_t *ptr() { return begin_; }

  size_t capacity() const { return buf_ ? buf_->capacity() : 0; }
  size_t size() const { return length_; }
  bool empty() const { return size() == 0; }

  bool contains(const uint8_t *data, uint32_t len) const { return ptr() <= data && data + len <= ptr() + capacity(); }

  void resetRange() {
    if (LIKELY(buf_ != nullptr)) {
      begin_ = buf_->ptr();
      length_ = buf_->capacity();
    }
  }

  bool advance(size_t n) {
    if (UNLIKELY(n > size())) {
      return false;
    }
    begin_ += n;
    length_ -= n;
    return true;
  }
  bool subtract(size_t n) {
    if (UNLIKELY(n > size())) {
      return false;
    }
    length_ -= n;
    return true;
  }

  RDMABuf subrange(size_t offset, size_t length) const {
    if (UNLIKELY(offset + length > size())) {
      return RDMABuf();
    }
    return RDMABuf(buf_, begin_ + offset, length);
  }

  RDMABuf first(size_t length) const { return subrange(0, length); }
  RDMABuf takeFirst(size_t length) {
    auto buf = first(length);
    advance(length);
    return buf;
  }

  RDMABuf last(size_t length) const {
    if (UNLIKELY(length > size())) {
      return RDMABuf();
    }
    return subrange(size() - length, length);
  }
  RDMABuf takeLast(size_t length) {
    auto buf = last(length);
    subtract(length);
    return buf;
  }

  RDMABufMR getMR(int dev) const {
    if (UNLIKELY(buf_ == nullptr)) {
      return RDMABufMR();
    }
    return buf_->getMR(dev);
  }

  RDMARemoteBuf toRemoteBuf() const {
    std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> rkeys;
    if (UNLIKELY(!buf_ || !buf_->getRkeys(rkeys))) {
      return RDMARemoteBuf();
    }
    return RDMARemoteBuf((uint64_t)begin_, length_, rkeys);
  }

  operator RDMARemoteBuf() { return toRemoteBuf(); }

  operator std::span<const uint8_t>() const { return {ptr(), size()}; }
  operator std::span<uint8_t>() { return {ptr(), size()}; }

  operator folly::MutableByteRange() { return {ptr(), size()}; }
  operator folly::ByteRange() const { return {ptr(), size()}; }

  bool operator==(const RDMABuf &o) const = default;

 private:
  friend class RDMABufPool;

  class Inner : folly::MoveOnly {
   public:
    static constexpr int kAccessFlags =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_RELAXED_ORDERING;

    Inner(size_t capacity)
        : Inner(std::weak_ptr<RDMABufPool>(), capacity) {}

    Inner(std::weak_ptr<RDMABufPool> pool, size_t capacity)
        : pool_(std::move(pool)),
          ptr_(nullptr),
          capacity_(capacity),
          mrs_(),
          userBuffer_(false) {}

    Inner(uint8_t *buf, size_t len)
        : pool_(std::weak_ptr<RDMABufPool>()),
          ptr_(buf),
          capacity_(len),
          mrs_(),
          userBuffer_(true) {}

    Inner(Inner &&o)
        : pool_(std::move(o.pool_)),
          ptr_(std::exchange(o.ptr_, nullptr)),
          capacity_(std::exchange(o.capacity_, 0)),
          mrs_(std::exchange(o.mrs_, std::array<RDMABufMR, IBDevice::kMaxDeviceCnt>())),
          userBuffer_(std::exchange(o.userBuffer_, false)) {}

    ~Inner();

    static void deallocate(Inner *ptr);

    int init();
    int allocateMemory();
    int registerMemory();

    RDMABufMR getMR(int dev) const;
    bool getRkeys(std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> &rkeys) const;

    const uint8_t *ptr() const { return ptr_; }
    uint8_t *ptr() { return ptr_; }
    size_t capacity() const { return capacity_; }

   private:
    std::weak_ptr<RDMABufPool> pool_;
    uint8_t *ptr_;
    size_t capacity_;
    std::array<RDMABufMR, IBDevice::kMaxDeviceCnt> mrs_;
    bool userBuffer_;
  };

  static RDMABuf allocate(size_t size, std::weak_ptr<RDMABufPool> pool);

 public:
  RDMABuf(Inner *buf)
      : RDMABuf(std::shared_ptr<Inner>(buf, Inner::deallocate)) {}

  RDMABuf(std::shared_ptr<Inner> buf)
      : buf_(std::move(buf)) {
    begin_ = buf_->ptr();
    length_ = buf_->capacity();
  }

  RDMABuf(std::shared_ptr<Inner> buf, uint8_t *begin, size_t length)
      : buf_(std::move(buf)),
        begin_(begin),
        length_(length) {}

  static RDMABuf createFromUserBuffer(uint8_t *buf, size_t len);

 private:
  std::shared_ptr<Inner> buf_;
  uint8_t *begin_;
  size_t length_;
};

class RDMABufPool : public std::enable_shared_from_this<RDMABufPool>, folly::MoveOnly {
  struct PrivateTag {};

 public:
  RDMABufPool(PrivateTag, size_t bufSize, size_t bufCnt)
      : bufSize_(bufSize),
        bufAllocated_(0),
        sem_(bufCnt),
        mutex_(),
        freeList_() {}

  ~RDMABufPool();

  static std::shared_ptr<RDMABufPool> create(size_t bufSize, size_t bufCnt) {
    return std::make_shared<RDMABufPool>(PrivateTag(), bufSize, bufCnt);
  }

  CoTask<RDMABuf> allocate(std::optional<folly::Duration> timeout = std::nullopt);
  void deallocate(RDMABuf::Inner *buf);

  size_t bufSize() const { return bufSize_; }
  size_t freeCnt() const { return sem_.getAvailableTokens(); }
  size_t totalCnt() const { return sem_.getCapacity(); }

 private:
  size_t bufSize_;
  std::atomic<size_t> bufAllocated_;

  folly::fibers::Semaphore sem_;
  std::mutex mutex_;
  std::deque<RDMABuf::Inner *> freeList_;
};

}  // namespace hf3fs::net

template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::net::RDMARemoteBuf> {
  static constexpr auto serialize(const net::RDMARemoteBuf &buf, auto &out) {
    uint8_t len = 0;
    for (auto &rkey : buf.rkeys()) {
      if (rkey.devId == -1) {
        break;
      } else {
        ++len;
      }
    }

    for (int8_t i = len - 1; i >= 0; --i) {
      serde::serialize(buf.rkeys()[i].devId, out);
      serde::serialize(buf.rkeys()[i].rkey, out);
    }
    serde::serialize(len, out);

    serde::serialize(buf.size(), out);
    serde::serialize(buf.addr(), out);
  }

  static Result<Void> deserialize(net::RDMARemoteBuf &buf, auto &&in) {
    uint64_t addr;
    uint64_t size;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(addr, in));
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(size, in));

    int8_t len;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(len, in));

    auto rkeys = buf.rkeys();
    for (auto &rkey : rkeys) {
      if (len-- > 0) {
        RETURN_AND_LOG_ON_ERROR(serde::deserialize(rkey.rkey, in));
        RETURN_AND_LOG_ON_ERROR(serde::deserialize(rkey.devId, in));
      } else {
        rkey.devId = -1;
      }
    }
    buf = net::RDMARemoteBuf(addr, size, rkeys);
    return Void{};
  }

  static auto serializeReadable(const net::RDMARemoteBuf &buf, auto &out) {
    auto start = out.tableBegin(false);
    {
      out.key("addr");
      serde::serialize(buf.addr(), out);
      out.key("size");
      serde::serialize(buf.size(), out);

      out.key("rkeys");
      out.arrayBegin();
      {
        for (auto &rkey : buf.rkeys()) {
          if (rkey.devId == -1) {
            break;
          }

          auto start = out.tableBegin(false);
          out.key("rkey");
          serde::serialize(rkey.rkey, out);
          out.key("devId");
          serde::serialize(rkey.devId, out);
          out.tableEnd(start);
        }
      }
      out.arrayEnd(0);
    }
    out.tableEnd(start);
  };

  static std::string serdeToReadable(const net::RDMARemoteBuf &rdmabuf) { return serde::toJsonString(rdmabuf); }

  static Result<net::RDMARemoteBuf> serdeFromReadable(const std::string &str) {
    net::RDMARemoteBuf rdmabuf;
    auto result = serde::fromJsonString(rdmabuf, str);
    if (result) return rdmabuf;
    return makeError(result.error());
  }
};
