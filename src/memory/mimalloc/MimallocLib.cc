#include <cstdio>
#include <cstdlib>

#include "memory/common/MemoryAllocatorInterface.h"
#include "mimalloc.h"

namespace hf3fs::memory {

class MimallocMemoryAllocator : public MemoryAllocatorInterface {
 public:
  void logstatus(char *buf, size_t size) override {
    size_t elapsed_msecs, user_msecs, system_msecs, current_rss, peak_rss, current_commit, peak_commit, page_faults;

    mi_process_info(&elapsed_msecs,
                    &user_msecs,
                    &system_msecs,
                    &current_rss,
                    &peak_rss,
                    &current_commit,
                    &peak_commit,
                    &page_faults);

    std::snprintf(buf,
                  size,
                  "mimalloc enabled, "
                  "current_rss=%zu, peak_rss=%zu, current_commit=%zu, peak_commit=%zu, page_faults=%zu",
                  current_rss,
                  peak_rss,
                  current_commit,
                  peak_commit,
                  page_faults);
  }

  void *allocate(size_t size) override { return mi_malloc(size); }

  void deallocate(void *mem) override { return mi_free(mem); }

  void *memalign(size_t alignment, size_t size) override { return mi_memalign(alignment, size); }

  bool profiling(bool /*active*/, const char * /*prefix*/) override {
    fprintf(stderr, "Memory profile not supported by mimalloc\n");
    return true;
  }
};

}  // namespace hf3fs::memory

extern "C" {
hf3fs::memory::MemoryAllocatorInterface *getMemoryAllocator() {
  static hf3fs::memory::MimallocMemoryAllocator mimalloc;
  return &mimalloc;
}
}
