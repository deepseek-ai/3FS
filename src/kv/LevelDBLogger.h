#pragma once

#include "leveldb/env.h"

namespace hf3fs::kv {
class LevelDBLogger final : public leveldb::Logger {
 public:
  LevelDBLogger() = default;
  ~LevelDBLogger() final;

  void Logv(const char *format, std::va_list ap) final;
};
}  // namespace hf3fs::kv
