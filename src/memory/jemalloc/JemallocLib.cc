#include <cstdio>
#include <cstdlib>
#include <jemalloc/jemalloc.h>

#include "memory/common/MemoryAllocatorInterface.h"

namespace hf3fs::memory {

class JemallocMemoryAllocator : public MemoryAllocatorInterface {
 public:
  void logstatus(char *buf, size_t size) override {
    size_t allocated, active, metadata, resident, mapped, retained, epoch = 1;
    size_t len = sizeof(size_t);

    /*
      stats.allocated (size_t) r- [--enable-stats]
      Total number of bytes allocated by the application.

      stats.active (size_t) r- [--enable-stats]
      Total number of bytes in active pages allocated by the application. This is a multiple of the page size, and
      greater than or equal to stats.allocated. This does not include stats.arenas.<i>.pdirty, stats.arenas.<i>.pmuzzy,
      nor pages entirely devoted to allocator metadata.

      stats.metadata (size_t) r- [--enable-stats]
      Total number of bytes dedicated to metadata, which comprise base allocations used for bootstrap-sensitive
      allocator metadata structures (see stats.arenas.<i>.base) and internal allocations (see
      stats.arenas.<i>.internal). Transparent huge page (enabled with opt.metadata_thp) usage is not considered.

      stats.resident (size_t) r- [--enable-stats]
      Maximum number of bytes in physically resident data pages mapped by the allocator, comprising all pages dedicated
      to allocator metadata, pages backing active allocations, and unused dirty pages. This is a maximum rather than
      precise because pages may not actually be physically resident if they correspond to demand-zeroed virtual memory
      that has not yet been touched. This is a multiple of the page size, and is larger than stats.active.

      stats.mapped (size_t) r- [--enable-stats]
      Total number of bytes in active extents mapped by the allocator. This is larger than stats.active. This does not
      include inactive extents, even those that contain unused dirty pages, which means that there is no strict ordering
      between this and stats.resident.

      stats.retained (size_t) r- [--enable-stats]
      Total number of bytes in virtual memory mappings that were retained rather than being returned to the operating
      system via e.g. munmap(2) or similar. Retained virtual memory is typically untouched, decommitted, or purged, so
      it has no strongly associated physical memory (see extent hooks for details). Retained memory is excluded from
      mapped memory statistics, e.g. stats.mapped.
    */

    if (je_mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch)) == 0 &&
        je_mallctl("stats.allocated", &allocated, &len, nullptr, 0) == 0 &&
        je_mallctl("stats.active", &active, &len, nullptr, 0) == 0 &&
        je_mallctl("stats.metadata", &metadata, &len, nullptr, 0) == 0 &&
        je_mallctl("stats.resident", &resident, &len, nullptr, 0) == 0 &&
        je_mallctl("stats.mapped", &mapped, &len, nullptr, 0) == 0 &&
        je_mallctl("stats.retained", &retained, &len, nullptr, 0) == 0) {
      // Get the jemalloc config string from environment variable (for logging purposes only).
      const char *configStr = std::getenv("JE_MALLOC_CONF");
      std::snprintf(buf,
                    size,
                    "jemalloc enabled (JE_MALLOC_CONF=\"%s\", profiling=%s), "
                    "current allocated=%zu active=%zu metadata=%zu resident=%zu mapped=%zu, retained=%zu",
                    configStr == nullptr ? "(null)" : configStr,
                    profilingActive_ ? "active" : "inactive",
                    allocated,
                    active,
                    metadata,
                    resident,
                    mapped,
                    retained);
    }
  }

  void *allocate(size_t size) override { return je_malloc(size); }

  void deallocate(void *mem) override { return je_free(mem); }

  void *memalign(size_t alignment, size_t size) override { return je_memalign(alignment, size); }

  bool profiling(bool active, const char *prefix) override {
    /*
      prof.active (bool) rw [--enable-prof]
      Control whether sampling is currently active. See the opt.prof_active option for additional information, as well
      as the interrelated thread.prof.active mallctl.

      prof.dump (const char *) -w [--enable-prof]
      Dump a memory profile to the specified file, or if NULL is specified, to a file according to the pattern
      <prefix>.<pid>.<seq>.m<mseq>.heap, where <prefix> is controlled by the opt.prof_prefix and prof.prefix options.

      prof.prefix (const char *) -w [--enable-prof]
      Set the filename prefix for profile dumps. See opt.prof_prefix for the default setting. This can be useful to
      differentiate profile dumps such as from forked processes.
    */

    if (profilingActive_) {
      // first dump memory profiling

      if (prefix != nullptr) {
        auto err = je_mallctl("prof.prefix", nullptr, nullptr, &prefix, sizeof(prefix));

        if (err) {
          fprintf(stderr, "Failed to set profiling dump prefix: %s, error: %d\n", prefix, err);
          return false;
        }
      }

      auto err = je_mallctl("prof.dump", nullptr, nullptr, nullptr, 0);

      if (err) {
        fprintf(stderr, "Failed to dump memory profiling, prefix: %s, error: %d\n", prefix, err);
        return false;
      }

      fprintf(stderr, "Memory profiling result saved with prefix: %s\n", prefix);
    }

    if (profilingActive_ == active) {
      fprintf(stderr, "Memory profiling already %s\n", active ? "enabled" : "disabled");
      return true;
    }

    auto err = je_mallctl("prof.active", nullptr, nullptr, (void *)&active, sizeof(active));

    if (err) {
      fprintf(stderr, "Failed to %s memory profiling, error :%d\n", active ? "enable" : "disable", err);
      return false;
    }

    fprintf(stderr,
            "Memory profiling set from '%s' to '%s'\n",
            profilingActive_ ? "active" : "inactive",
            active ? "active" : "inactive");
    profilingActive_ = active;

    return true;
  }

 private:
  bool profilingActive_ = false;
};

}  // namespace hf3fs::memory

extern "C" {
hf3fs::memory::MemoryAllocatorInterface *getMemoryAllocator() {
  static hf3fs::memory::JemallocMemoryAllocator jemalloc;
  return &jemalloc;
}
}
