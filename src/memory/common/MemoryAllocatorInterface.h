#pragma once
#include <cstddef>

#define GET_MEMORY_ALLOCATOR_FUNC_NAME "getMemoryAllocator"

namespace hf3fs::memory {

class MemoryAllocatorInterface {
 public:
  virtual ~MemoryAllocatorInterface() = default;
  virtual void *allocate(size_t size) = 0;
  virtual void deallocate(void *mem) = 0;
  virtual void *memalign(size_t alignment, size_t size) = 0;
  virtual void logstatus(char *buf, size_t size) = 0;
  virtual bool profiling(bool active, const char *prefix) = 0;
};

using GetMemoryAllocatorFunc = MemoryAllocatorInterface *(*)();

}  // namespace hf3fs::memory
