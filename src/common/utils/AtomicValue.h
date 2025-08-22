#pragma once

#include <atomic>

namespace hf3fs {

// Wrapper of atomic value, support copy.
template <class T>
class AtomicValue {
 public:
  AtomicValue() = default;
  explicit AtomicValue(T value)
      : value_(value) {}
  AtomicValue(const AtomicValue &o)
      : value_(o.value()) {}

  T value(std::memory_order order = std::memory_order_seq_cst) const { return value_.load(order); }
  void setValue(T value, std::memory_order order = std::memory_order_seq_cst) { value_.store(value, order); }

  T load(std::memory_order order = std::memory_order_seq_cst) const { return value_.load(order); }
  void store(T value, std::memory_order order = std::memory_order_seq_cst) { value_.store(value, order); }

  operator T() const { return value(); }
  AtomicValue &operator=(T value) {
    setValue(value);
    return *this;
  }
  AtomicValue &operator=(const AtomicValue &o) {
    setValue(o.value());
    return *this;
  }

 private:
  std::atomic<T> value_;
};

}  // namespace hf3fs
