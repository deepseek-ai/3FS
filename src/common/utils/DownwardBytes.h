#pragma once

#include <cstring>
#include <exception>
#include <folly/Likely.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <stdexcept>

namespace hf3fs {

template <class Allocator>
class DownwardBytes {
 public:
  DownwardBytes() = default;
  DownwardBytes(const DownwardBytes &o)
      : offset_(o.offset_),
        capacity_(o.capacity_),
        data_(Allocator::allocate(capacity_)) {
    std::memcpy(this->data(), o.data(), o.size());
  }
  DownwardBytes(DownwardBytes &&o)
      : offset_(std::exchange(o.offset_, 0)),
        capacity_(std::exchange(o.capacity_, 0)),
        data_(std::exchange(o.data_, nullptr)) {}
  ~DownwardBytes() { reset(); }

  DownwardBytes &operator=(const DownwardBytes &o) {
    if (std::addressof(o) != this) {
      reserve(o.size());
      offset_ = capacity_ - o.size();
      std::memcpy(this->data(), o.data(), o.size());
    }
    return *this;
  }
  DownwardBytes &operator=(DownwardBytes &&o) {
    if (std::addressof(o) != this) {
      offset_ = std::exchange(o.offset_, 0);
      capacity_ = std::exchange(o.capacity_, 0);
      data_ = std::exchange(o.data_, nullptr);
    }
    return *this;
  }

  auto size() const { return capacity_ - offset_; }
  auto capacity() const { return capacity_; }
  auto data() { return data_ + offset_; }
  auto data() const { return data_ + offset_; }
  auto &operator[](size_t idx) { return data()[idx]; }
  auto &operator[](size_t idx) const { return data()[idx]; }
  operator std::string_view() const { return std::string_view{reinterpret_cast<const char *>(data()), size()}; }

  void append(auto *data, uint32_t size) {
    reserve(this->size() + size);
    offset_ -= size;
    std::memcpy(this->data(), data, size);
  }

  void append(uint8_t value) {
    reserve(this->size() + 1);
    offset_ -= 1;
    *data() = value;
  }

  void reset() {
    if (data_) {
      Allocator::deallocate(std::exchange(data_, nullptr), std::exchange(capacity_, 0));
    }
    offset_ = 0;
  }

  void reserve(uint32_t size) {
    if (UNLIKELY(size > capacity_)) {
      auto newCap = std::max(size, capacity_ * 2);
      auto newBuf = Allocator::allocate(newCap);
      if (auto length = this->size()) {
        auto newOffset = newCap - length;
        std::memcpy(newBuf + newOffset, this->data(), length);
        offset_ = newOffset;
      } else {
        offset_ = newCap;
      }
      Allocator::deallocate(data_, capacity_);
      capacity_ = newCap;
      data_ = newBuf;
    }
  }

  uint8_t *release(uint32_t &offset, uint32_t &capacity) {
    offset = std::exchange(offset_, 0);
    capacity = std::exchange(capacity_, 0);
    return std::exchange(data_, nullptr);
  }

  std::string toString() const { return std::string{*this}; }

 private:
  uint32_t offset_ = Allocator::kDefaultSize;
  uint32_t capacity_ = Allocator::kDefaultSize;
  std::uint8_t *data_ = Allocator::allocate(Allocator::kDefaultSize);
};

template <>
class DownwardBytes<void> {
 public:
  auto size() const { return length_; }
  auto &operator[](size_t) { return dummy_; }
  void append(auto *, uint32_t size) { length_ += size; }

 private:
  uint32_t length_ = 0;
  uint32_t dummy_ = 0;
};

struct UserBufferAllocator {};

template <>
class DownwardBytes<UserBufferAllocator> {
 public:
  DownwardBytes() = default;

  void setBuffer(std::uint8_t *data, uint32_t capacity) {
    data_ = data;
    offset_ = capacity_ = capacity;
  }

  auto size() const { return capacity_ - offset_; }
  auto capacity() const { return capacity_; }
  auto data() { return data_ + offset_; }
  auto data() const { return data_ + offset_; }
  auto &operator[](size_t idx) { return data()[idx]; }
  auto &operator[](size_t idx) const { return data()[idx]; }
  operator std::string_view() const { return std::string_view{reinterpret_cast<const char *>(data()), size()}; }

  void append(auto *data, uint32_t size) {
    if (offset_ < size) {
      XLOGF(FATAL, "insufficient capacity! cap {} offset {} size {}", capacity_, offset_, size);
    }
    offset_ -= size;
    std::memcpy(this->data(), data, size);
  }

  void append(uint8_t value) {
    if (offset_ < 1) {
      XLOGF(FATAL, "insufficient capacity! cap {} offset {} size {}", capacity_, offset_, 1);
    }
    offset_ -= 1;
    *data() = value;
  }

  std::string toString() const { return std::string{*this}; }

 private:
  uint32_t offset_{};
  uint32_t capacity_{};
  std::uint8_t *data_{};
};

}  // namespace hf3fs
