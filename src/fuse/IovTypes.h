#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>

#include "common/utils/Uuid.h"
#include "lib/common/Shm.h"
#ifdef HF3FS_GDR_ENABLED
#include "lib/common/GpuShm.h"
#endif

namespace hf3fs::fuse {

#ifdef HF3FS_GDR_ENABLED
using IoBufForIO = std::variant<lib::ShmBufForIO, lib::GpuShmBufForIO>;

inline uint8_t *ioBufPtr(const IoBufForIO &buf) {
  return std::visit([](const auto &b) -> uint8_t * { return b.ptr(); }, buf);
}
#else
using IoBufForIO = lib::ShmBufForIO;

inline uint8_t *ioBufPtr(const IoBufForIO &buf) { return buf.ptr(); }
#endif

struct IovLookupRequest {
  Uuid id;
  size_t offset;
  size_t length;
};

}  // namespace hf3fs::fuse
