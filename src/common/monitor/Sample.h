#pragma once

#include <folly/hash/Hash.h>
#include <folly/sorted_vector_types.h>
#include <functional>
#include <string>
#include <utility>
#include <variant>

#include "common/serde/Serde.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::monitor {

class TagSet {
 public:
  using Map = folly::sorted_vector_map<std::string, std::string>;
  using Iterator = Map::iterator;
  using ConstIterator = Map::const_iterator;

  TagSet() = default;
  TagSet(std::initializer_list<Map::value_type> init)
      : tag_set_(init) {}

  void addTag(std::string tagName, std::string tagValue) { tag_set_.emplace(tagName, tagValue); }

  static TagSet create(std::string tagName, std::string tagValue) {
    TagSet ts;
    ts.addTag(std::move(tagName), std::move(tagValue));
    return ts;
  }

  TagSet newTagSet(std::string tagName, std::string tagValue) const {
    TagSet newTagSet(*this);
    newTagSet.addTag(tagName, tagValue);
    return newTagSet;
  }

  std::map<String, String> asMap() const { return {begin(), end()}; }

  size_t size() const { return tag_set_.size(); }

  Iterator begin() { return tag_set_.begin(); }
  Iterator end() { return tag_set_.end(); }

  ConstIterator begin() const { return tag_set_.begin(); }
  ConstIterator end() const { return tag_set_.end(); }

  ConstIterator cbegin() const { return tag_set_.cbegin(); }
  ConstIterator cend() const { return tag_set_.cend(); }

  Iterator find(const std::string &key) { return tag_set_.find(key); }
  ConstIterator find(const std::string &key) const { return tag_set_.find(key); }

  bool contains(const std::string &key) const { return tag_set_.find(key) != tag_set_.end(); }
  std::string &operator[](const std::string &key) { return tag_set_[key]; }

  void traverseTags(std::function<void(const std::string &, const std::string &)> fn) const {
    for (auto &tag : tag_set_) {
      fn(tag.first, tag.second);
    }
  }

  friend bool operator==(const TagSet &a, const TagSet &b) { return a.tag_set_ == b.tag_set_; }

 private:
  SERDE_CLASS_FIELD(tag_set, Map{});
};

inline TagSet instanceTagSet(std::string instance) {
  constexpr std::string_view kMetricInstanceTag = "instance";
  return {{std::string(kMetricInstanceTag), instance}};
}

inline TagSet threadTagSet(const std::string_view &threadName) {
  constexpr std::string_view kMetricThreadTag = "thread";
  return {{std::string(kMetricThreadTag), std::string(threadName)}};
}

struct Distribution {
  constexpr static auto kTypeNameForSerde = "dist";

  SERDE_STRUCT_FIELD(cnt, double{});
  SERDE_STRUCT_FIELD(sum, double{});
  SERDE_STRUCT_FIELD(min, double{});
  SERDE_STRUCT_FIELD(max, double{});
  SERDE_STRUCT_FIELD(p50, double{});
  SERDE_STRUCT_FIELD(p90, double{});
  SERDE_STRUCT_FIELD(p95, double{});
  SERDE_STRUCT_FIELD(p99, double{});

 public:
  double mean() const { return cnt == 0.0 ? 0.0 : sum / cnt; }
};

struct Sample {
  SERDE_STRUCT_FIELD(name, std::string{});
  SERDE_STRUCT_FIELD(tags, TagSet{});
  SERDE_STRUCT_FIELD(timestamp, UtcTime{});
  SERDE_STRUCT_FIELD(value, (std::variant<int64_t, Distribution>{}));

 public:
  bool isNumber() const { return std::holds_alternative<int64_t>(value); }
  int64_t number() const { return std::get<int64_t>(value); }

  bool isDistribution() const { return std::holds_alternative<Distribution>(value); }
  Distribution &dist() { return std::get<Distribution>(value); }
  const Distribution &dist() const { return std::get<Distribution>(value); }
};

}  // namespace hf3fs::monitor

template <>
struct std::hash<hf3fs::monitor::TagSet> {
  std::size_t operator()(const hf3fs::monitor::TagSet &tag_set) const {
    return folly::hash::hash_range(tag_set.cbegin(), tag_set.cend());
  }
};
