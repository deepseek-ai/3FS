#pragma once

#include <shared_mutex>

namespace hf3fs {

#ifndef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
#error "only support GNU pthread rwlock!"
#endif

class FairSharedMutex : public std::shared_mutex {
 public:
  FairSharedMutex() {
    *reinterpret_cast<pthread_rwlock_t *>(native_handle()) = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;
  }
};

}  // namespace hf3fs
