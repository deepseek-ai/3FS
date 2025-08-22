#pragma once

#include <list>

#include "common/utils/RobinHood.h"

namespace hf3fs {

template <class K, class V>
class LruCache {
 public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<key_type, mapped_type>;
  using list_type = std::list<value_type>;
  using size_type = typename list_type::size_type;
  using iterator = typename list_type::iterator;
  using const_iterator = typename list_type::const_iterator;

 public:
  LruCache(size_t capacity, bool autoRemove = true)
      : capacity_(capacity),
        autoRemove_(autoRemove) {}

  iterator begin() { return least_.begin(); }
  const_iterator begin() const { return least_.begin(); }
  iterator end() { return least_.end(); }
  const_iterator end() const { return least_.end(); }
  bool empty() const { return least_.empty(); }
  size_t size() const { return least_.size(); }
  value_type &front() { return least_.front(); }
  const value_type &front() const { return least_.front(); }
  value_type &back() { return least_.back(); }
  const value_type &back() const { return least_.back(); }
  size_t getMaxSize() const { return capacity_; }
  void setMaxSize(size_t capacity) { capacity_ = capacity; }
  void clear() { used_.clear(), least_.clear(); }

  iterator find(const key_type &key) {
    auto it = used_.find(key);
    if (it == used_.end()) {
      return end();
    }
    return it->second;
  }
  const_iterator find(const key_type &key) const {
    auto it = used_.find(key);
    if (it == used_.end()) {
      return end();
    }
    return it->second;
  }

  void promote(const_iterator it) { least_.splice(least_.begin(), least_, it); }
  void obsolete(const_iterator it) { least_.splice(least_.end(), least_, it); }

  iterator erase(const key_type &key) {
    auto it = used_.find(key);
    if (it != used_.end()) {
      return erase(it->second);
    }
    return end();
  }
  iterator erase(const_iterator it) {
    used_.erase(it->first);
    return least_.erase(it);
  }

  size_t evictObsoleted() {
    if (least_.size() <= capacity_) {
      return 0;
    }
    auto evictingSize = least_.size() - capacity_;
    for (size_t i = 0; i < evictingSize; ++i) {
      used_.erase(back().first);
      least_.pop_back();
    }
    return evictingSize;
  }
  template <class Predicate>
  size_t evictObsoletedIf(Predicate &&p) {
    if (least_.size() <= capacity_) {
      return 0;
    }
    auto evictingSize = least_.size() - capacity_;
    for (size_t i = 0; i < evictingSize && p(back().first, back().second); ++i) {
      used_.erase(back().first);
      least_.pop_back();
    }

    return evictingSize;
  }

  template <class T>
  std::pair<iterator, bool> emplace(const key_type &key, T &&value) {
    auto [it, succ] = used_.emplace(key, iterator{});
    if (succ) {
      least_.emplace_front(key, std::forward<T>(value));
      it->second = least_.begin();
      if (autoRemove_) {
        evictObsoleted();
      }
    } else {
      promote(it->second);
    }
    return std::make_pair(it->second, succ);
  }

  mapped_type &operator[](const key_type &key) { return emplace(key, mapped_type{}).first->second; }

 private:
  size_t capacity_;
  bool autoRemove_;
  list_type least_;
  robin_hood::unordered_map<key_type, iterator> used_;
};

}  // namespace hf3fs
