#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "MemoryAllocatorInterface.h"

namespace hf3fs::memory {

#ifdef OVERRIDE_CXX_NEW_DELETE

void *allocate(size_t size);

void deallocate(void *mem);

void *memalign(size_t alignment, size_t size);

void logstatus(char *buf, size_t size);

bool profiling(bool active, const char *prefix);

void shutdown();

#else

inline void *allocate(size_t size) { return std::malloc(size); }

inline void deallocate(void *mem) { return std::free(mem); }

// A copy of folly::aligned_malloc() from third_party/folly/folly/Memory.h
inline void *memalign(size_t alignment, size_t size) {
  // use posix_memalign, but mimic the behaviour of memalign
  void *ptr = nullptr;
  int rc = posix_memalign(&ptr, alignment, size);
  return rc == 0 ? (errno = 0, ptr) : (errno = rc, nullptr);
}

inline void logstatus(char *buf, size_t size) { std::snprintf(buf, size, "C++ new/delete not overridden"); }

inline bool profiling(bool /*active*/, const char * /*prefix*/) { return false; }

inline void shutdown() {}

#endif

}  // namespace hf3fs::memory
