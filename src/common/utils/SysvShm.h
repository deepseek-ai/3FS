#pragma once

#include <span>
#include <sys/shm.h>

#include "common/utils/Path.h"
#include "common/utils/Result.h"

namespace hf3fs {
class SysvShm {
 public:
  ~SysvShm();

  static Result<std::shared_ptr<SysvShm>> create(const Path &path, size_t size, int flags);
  std::span<uint8_t> getBuf() const;
  Result<shmid_ds> getStat() const;
  Result<Void> detach();
  Result<Void> remove();

 protected:
  SysvShm();

  int id_ = -1;
  uint8_t *ptr_ = nullptr;
  size_t size_ = 0;
  bool removed_ = false;
};
}  // namespace hf3fs
