#include "Shm.h"

#include <fcntl.h>
#include <folly/ScopeGuard.h>
#include <numa.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

namespace hf3fs::lib {
ShmBuf::ShmBuf(const Path &p, size_t sz, size_t bsz, int numa, meta::Uid u, int pr, int pp)
    : path(p),
      size(sz),
      blockSize(bsz ? bsz : sz),
      off(0),
      user(u),
      pid(pr),
      ppid(pp),
      id(Uuid::random()),
      owner_(true),
      numaNode_(numa) {
  if (!sz || bsz > sz) {
    throw std::invalid_argument(fmt::format("invalid block size {} total bytes {}", bsz, sz));
  }

  mapBuf();

  if (numaNode_ >= 0) {
    numa_tonode_memory(bufStart, size, numaNode_);
  }
}

ShmBuf::ShmBuf(const Path &p, off_t o, size_t sz, size_t bsz, Uuid u)
    : path(p),
      size(sz),
      blockSize(bsz ? bsz : sz),
      off(o),
      //      user(0),
      id(u),
      owner_(false),
      numaNode_(-1) {
  if (!sz || bsz > sz) {
    throw std::invalid_argument(fmt::format("invalid block size {} total bytes {}", bsz, sz));
  }

  XLOGF(DBG, "this {} buf start {} off {}", (void *)this, (void *)bufStart, off);

  mapBuf();

  XLOGF(DBG, "this {} buf start {} off {}", (void *)this, (void *)bufStart, off);
}

ShmBuf::~ShmBuf() {
  XLOGF(DBG, "calling dtor of shm {}", (void *)this);

  deregisterForIO();

  // if (owner_) {
  //   auto res = ftruncate(fd_, 0);
  //   XLOGF_IF(ERR, res < 0, "ftruncate shm file to 0 failed {}", res);
  // }

  unmapBuf();
}

void ShmBuf::deregisterForIO() {
  memhs_.clear();
}

CoTryTask<void> ShmBuf::registerForIO(storage::client::StorageClient &sc, std::function<void()> recordMetrics) {
  // Keep the handles local so every registered segment is deregistered on failure.
  std::vector<storage::client::IOBuffer> handles;
  handles.reserve(1 + (size - 1) / blockSize);
  for (size_t off = 0; off < size;) {
    const auto len = std::min(size - off, blockSize);
    auto res = sc.registerIOBuffer(bufStart + off, len);
    if (!res) {
      co_return MAKE_ERROR_F(res.error().code(),
                            "failed to register IOV segment at offset {} ({} bytes): {}",
                            off,
                            len,
                            res.error());
    }
    handles.push_back(std::move(*res));
    recordMetrics();
    off += len;
  }
  memhs_ = std::move(handles);
  co_return Void{};
}

void ShmBuf::unmapBuf() {
  if (bufStart) {
    munmap(bufStart, size);

    if (owner_) {
      shm_unlink(path.c_str());
    }

    bufStart = nullptr;
  }
}

storage::client::IOBuffer *ShmBuf::memh(size_t off) {
  auto idx = off / blockSize;
  return idx < memhs_.size() ? &memhs_[idx] : nullptr;
}

void ShmBuf::mapBuf() {
  auto fd = shm_open(path.c_str(), O_RDWR | (owner_ ? O_CREAT | O_EXCL : 0), 0666);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "shm_open");
  }

  SCOPE_EXIT { close(fd); };
  SCOPE_FAIL {
    if (owner_) {
      shm_unlink(path.c_str());
    }
  };

  if (owner_ && ftruncate(fd, size) < 0) {
    throw std::system_error(errno, std::generic_category(), "ftruncate");
  }
  auto *buf = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
  if (buf == MAP_FAILED) {
    throw std::system_error(errno, std::generic_category(), "mmap");
  }
  bufStart = static_cast<uint8_t *>(buf);

  XLOGF(DBG, "this {} owner {} buf start {} size {} off {}", (void *)this, owner_, (void *)bufStart, size, off);
}
}  // namespace hf3fs::lib
