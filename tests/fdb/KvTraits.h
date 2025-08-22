#pragma once

#include <cstdint>
#include <map>

#include "common/kv/mem/MemKVEngine.h"
#include "fdb/FDBKVEngine.h"

namespace hf3fs::testing {
template <typename KV>
struct KVTrait {};

template <>
struct KVTrait<kv::mem::MemKV> {
  using Engine = kv::MemKVEngine;
  using State = uint64_t;
  using Transaction = kv::MemTransaction;

  static constexpr bool kSupportConflictCheck = true;
};

template <>
struct KVTrait<kv::fdb::DB> {
  using Engine = kv::FDBKVEngine;
  using State = std::map<String, String>;
  using Transaction = kv::FDBTransaction;

  static constexpr bool kSupportConflictCheck = false;
};

}  // namespace hf3fs::testing
