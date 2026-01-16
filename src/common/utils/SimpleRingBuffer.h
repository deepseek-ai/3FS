#pragma once

#include <cstddef>
#include <concepts>
#include <memory>
#include <utility>

namespace hf3fs {

template <typename T, typename Allocator = std::allocator<T>>
class SimpleRingBuffer {
 public:
  using allocator_type = Allocator;
  using alloc_traits = std::allocator_traits<Allocator>;

  explicit SimpleRingBuffer(size_t capacity, const Allocator& alloc = Allocator())
      : cap_(capacity),
        head_(0),
        tail_(0),
        alloc_(alloc) {
    buffer_ = alloc_traits::allocate(alloc_, cap_);
  }

  ~SimpleRingBuffer() {
    clear();
    alloc_traits::deallocate(alloc_, buffer_, cap_);
  }

  SimpleRingBuffer(const SimpleRingBuffer&) = delete;
  SimpleRingBuffer& operator=(const SimpleRingBuffer&) = delete;
  // TODO: support move, after making cap_ non-const
  SimpleRingBuffer(SimpleRingBuffer&&) = delete;
  SimpleRingBuffer& operator=(SimpleRingBuffer&&) = delete;

  template <typename U>
  requires(std::constructible_from<T, U>)
  bool push(U &&v) {
    if (full()) return false;
    alloc_traits::construct(alloc_, addr(head_), std::forward<U>(v));
    ++head_;
    return true;
  }

  template <typename... Args>
  requires(std::constructible_from<T, Args...>)
  bool emplace(Args &&... args) {
    if (full()) return false;
    alloc_traits::construct(alloc_, addr(head_), std::forward<Args>(args)...);
    ++head_;
    return true;
  }

  bool pop() {
    if (empty()) return false;
    alloc_traits::destroy(alloc_, addr(tail_));
    ++tail_;
    return true;
  }

  bool pop(T &v) {
    if (empty()) return false;
    T* tail = addr(tail_);
    v = std::move(*tail);
    alloc_traits::destroy(alloc_, tail);
    ++tail_;
    return true;
  }

  size_t size() const { return head_ - tail_; }
  bool empty() const { return head_ == tail_; }
  bool full() const { return size() == cap_; }

  void clear() {
    while (!empty()) pop();
    head_ = tail_ = 0;
  }

  allocator_type get_allocator() const { return alloc_; }

  // TODO: should be std::random_access_iterator_tag
  template <bool Const>
  class iterator_base {
   public:
    using BufT = std::conditional_t<Const, const SimpleRingBuffer, SimpleRingBuffer>;
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
      return *this;
    }

    iterator_base operator++(int) {
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
  T* addr(size_t pos) {
    return buffer_ + (pos % cap_);
  }
  const T* addr(size_t pos) const {
    return buffer_ + (pos % cap_);
  }

  const size_t cap_;
  size_t head_;
  size_t tail_;
  [[no_unique_address]] Allocator alloc_;
  T* buffer_;
};

}  // namespace hf3fs
