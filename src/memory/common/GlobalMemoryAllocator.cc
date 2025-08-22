#include "GlobalMemoryAllocator.h"

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <folly/logging/xlog.h>
#include <mutex>

#include "AllocatedMemoryCounter.h"
#include "common/monitor/Monitor.h"

namespace hf3fs::memory {

#ifdef OVERRIDE_CXX_NEW_DELETE

static std::once_flag gInitOnce;
static bool gAllocatorInited = false;
static MemoryAllocatorInterface *gAllocator = nullptr;
static thread_local hf3fs::memory::AllocatedMemoryCounter gMemCounter;

static const size_t kHeaderSize = alignof(max_align_t);
static_assert(kHeaderSize >= 2 * sizeof(size_t), "kHeaderSize < 2 * sizeof(size_t)");
static_assert(sizeof(void *) <= sizeof(size_t), "sizeof(void *) > sizeof(size_t)");

static void loadMemoryAllocatorLib() {
  char logBuf[512] = {0};
  void *mallocLib = nullptr;
  GetMemoryAllocatorFunc getMemoryAllocatorFunc = nullptr;

  // If environment variable is not defined, fall back to system malloc.
  const char *mallocLibPath = std::getenv("MEMORY_ALLOCATOR_LIB_PATH");
  if (mallocLibPath == nullptr || mallocLibPath[0] == '\0') {
#ifndef NDEBUG
    fprintf(stderr, "No memory allocator lib path specified\n");
#endif
    goto exit;
  }

  mallocLib = ::dlopen(mallocLibPath, RTLD_NOW | RTLD_GLOBAL);
  if (mallocLib == nullptr) {
    fprintf(stderr, "Cannot load dynamic library, error: %s, path: %s\n", dlerror(), mallocLibPath);
    goto exit;
  }

  getMemoryAllocatorFunc = (GetMemoryAllocatorFunc)::dlsym(mallocLib, GET_MEMORY_ALLOCATOR_FUNC_NAME);
  if (getMemoryAllocatorFunc == nullptr) {
    fprintf(stderr,
            "Cannot find function '%s' in dynamic library, error: %s, path: %s\n",
            GET_MEMORY_ALLOCATOR_FUNC_NAME,
            dlerror(),
            mallocLibPath);
    goto exit;
  }

  gAllocator = getMemoryAllocatorFunc();
  if (gAllocator == nullptr) {
    fprintf(stderr,
            "Cannot get memory allocator by calling '%s', path: %s\n",
            GET_MEMORY_ALLOCATOR_FUNC_NAME,
            mallocLibPath);
    goto exit;
  }

  gAllocator->logstatus(logBuf, sizeof(logBuf));
  fprintf(stderr, "Memory allocator loaded: %s\n", logBuf);

exit:
  // If failed to get allocator, free dynamic library before return.
  if (gAllocator == nullptr && mallocLib != nullptr) {
    fprintf(stderr, "Memory allocator unloaded: %s\n", mallocLibPath);
    ::dlclose(mallocLib);
  }
}

void *allocate(size_t size) {
  if (!gAllocatorInited) {
    std::call_once(gInitOnce, loadMemoryAllocatorLib);
    gAllocatorInited = true;
  }

#ifdef SAVE_ALLOCATE_SIZE
  const size_t headerSize = kHeaderSize;
#else
  const size_t headerSize = 0;
#endif

  const size_t allocateSize = size + headerSize;
  void *mem;

  if (gAllocator == nullptr)
    mem = std::malloc(allocateSize);
  else
    mem = gAllocator->allocate(allocateSize);

  assert(mem != nullptr);
  if (mem == nullptr) return nullptr;

  gMemCounter.add(allocateSize);

#ifdef SAVE_ALLOCATE_SIZE
  void *header = (uint8_t *)mem;
  auto sizePtr = static_cast<size_t *>(header);
  auto memPtr = static_cast<size_t *>(header) + 1;
  *sizePtr = allocateSize;
  *memPtr = size_t(mem);
  return static_cast<uint8_t *>(mem) + headerSize;
#else
  return mem;
#endif
}

void deallocate(void *mem) {
  if (mem == nullptr) return;

#ifdef SAVE_ALLOCATE_SIZE
  uint8_t *header = static_cast<uint8_t *>(mem) - kHeaderSize;
  auto sizePtr = reinterpret_cast<size_t *>(header);
  auto memPtr = reinterpret_cast<size_t *>(header) + 1;
  mem = reinterpret_cast<uint8_t *>(*memPtr);
  assert(mem <= header);
  size_t allocateSize = *sizePtr;
  gMemCounter.sub(allocateSize);
#endif

  if (gAllocator == nullptr) {
    return free(mem);
  }

  return gAllocator->deallocate(mem);
}

void *memalign(size_t alignment, size_t size) {
  if (!gAllocatorInited) {
    std::call_once(gInitOnce, loadMemoryAllocatorLib);
    gAllocatorInited = true;
  }

#ifdef SAVE_ALLOCATE_SIZE
  const size_t alignedHeaderSize = std::max(alignment, kHeaderSize);
#else
  const size_t alignedHeaderSize = 0;
#endif

  const size_t allocateSize = size + alignedHeaderSize;
  void *mem;

  if (gAllocator == nullptr)
    mem = std::aligned_alloc(alignment, allocateSize);
  else
    mem = gAllocator->memalign(alignment, allocateSize);

  assert(mem != nullptr);
  if (mem == nullptr) return nullptr;

  gMemCounter.add(allocateSize);

#ifdef SAVE_ALLOCATE_SIZE
  void *header = (uint8_t *)mem + (alignedHeaderSize - kHeaderSize);
  auto sizePtr = static_cast<size_t *>(header);
  auto memPtr = static_cast<size_t *>(header) + 1;
  *sizePtr = allocateSize;
  *memPtr = size_t(mem);
  return static_cast<uint8_t *>(mem) + alignedHeaderSize;
#else
  return mem;
#endif
}

void logstatus(char *buf, size_t size) {
  if (gAllocator)
    gAllocator->logstatus(buf, size);
  else
    snprintf(buf, size, "Memory allocator unloaded");
}

bool profiling(bool active, const char *prefix) {
  if (gAllocator)
    return gAllocator->profiling(active, prefix);
  else
    return false;
}

void shutdown() {
  if (gAllocator) {
    char logBuf[512] = {0};
    gAllocator->profiling(false /*active*/, nullptr /*prefix*/);
    gAllocator->logstatus(logBuf, sizeof(logBuf));
    fprintf(stderr, "Memory allocator shutdown: %s\n", logBuf);
  }

  memory::AllocatedMemoryCounter::shutdown();
}

#endif  // OVERRIDE_CXX_NEW_DELETE

}  // namespace hf3fs::memory
