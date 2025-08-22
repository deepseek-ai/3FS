#pragma once

#include <cstddef>
#include <vector>

namespace hf3fs {

template <typename T>
class SimpleRingBuffer {
 public:
  explicit SimpleRingBuffer(size_t capacity)
      : cap_(capacity),
        head_(0),
        tail_(0) {
    buffer_.resize(capacity * sizeof(T));
  }

  template <typename U>
  requires(std::same_as<T, std::decay_t<U>>) bool push(U &&v) {
    if (full()) return false;
    new (addr(head_)) T(std::move(v));
    ++head_;
    return true;
  }

  template <typename... Args>
  bool emplace(Args &&...args) {
    if (full()) return false;
    new (addr(head_)) T(std::forward<Args>(args)...);
    ++head_;
    return true;
  }

  bool pop() {
    if (empty()) return false;
    auto *tail = addr(tail_);
    tail->~T();
    ++tail_;
    return true;
  }

  bool pop(T &v) {
    if (empty()) return false;
    auto *tail = addr(tail_);
    v = std::move(*tail);
    tail->~T();
    ++tail_;
    return true;
  }

  bool empty() const { return head_ == tail_; }

  bool full() const { return head_ == tail_ + cap_; }

  // TODO: should be std::random_access_iterator_tag
  template <bool Const>
  class iterator_base {
   public:
    using BufT = std::conditional_t<Const, const SimpleRingBuffer<T>, SimpleRingBuffer<T>>;
    using ValueT = std::conditional_t<Const, const T, T>;
    using difference_type = std::ptrdiff_t;
    using value_type = ValueT;
    using pointer = ValueT *;
    using reference = ValueT &;
    using iterator_category = std::input_iterator_tag;
    iterator_base(BufT *rb, size_t pos)
        : rb_(rb),
          pos_(pos) {}

    iterator_base &operator++() {
      ++pos_;
      pos_ %= rb_->cap_;
      return *this;
    }

    iterator_base &operator++(int) {
      auto old = *this;
      ++*this;
      return old;
    }

    ValueT *operator->() const { return rb_->addr(pos_); }
    ValueT &operator*() const { return *rb_->addr(pos_); }

    bool operator==(const iterator_base &other) const { return rb_ == other.rb_ && pos_ == other.pos_; }

    bool operator!=(const iterator_base &other) const { return !(*this == other); }

   private:
    BufT *rb_;
    size_t pos_;
  };

  using iterator = iterator_base<false>;
  using const_iterator = iterator_base<true>;

  iterator begin() { return iterator(this, tail_); }
  iterator end() { return iterator(this, head_); }

  const_iterator begin() const { return const_iterator(this, tail_); }
  const_iterator end() const { return const_iterator(this, head_); }

  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

 private:
  T *addr(size_t pos) {
    pos %= cap_;
    return std::launder(reinterpret_cast<T *>(buffer_.data() + pos * sizeof(T)));
  }

  const size_t cap_;
  size_t head_;
  size_t tail_;
  std::vector<std::byte> buffer_;
};

}  // namespace hf3fs
