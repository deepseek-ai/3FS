#pragma once

#include <forward_list>
#include <shared_mutex>

#include "PerProcTable.h"
#include "client/storage/StorageClient.h"
#include "common/utils/Path.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::lib {

struct IorAttrs {
  int priority = 1;
  Duration timeout{Duration::zero()};
  uint64_t flags = 0;
};

struct ShmBuf {
  ShmBuf(const Path &p, size_t sz, size_t bsz, int numa, meta::Uid u, int pid, int ppid);
  ShmBuf(const Path &p, off_t o, size_t sz, size_t bsz, Uuid u);
  ~ShmBuf();

  CoTask<void> registerForIO(folly::Executor::KeepAlive<> exec,
                             storage::client::StorageClient &sc,
                             std::function<void()> &&recordMetrics);
  CoTask<std::shared_ptr<storage::client::IOBuffer>> memh(size_t off);

  bool checkId(const Uuid &uid) const { return id == uid; }

  CoTask<void> deregisterForIO();
  void unmapBuf();
  bool maybeUnlinkShm() {
    if (owner_) {
      shm_unlink(path.c_str());
    }

    return owner_;
  }

  Path path;
  uint8_t *bufStart{nullptr};
  size_t size{0};
  size_t blockSize{0};

  // for client lib
  off_t off;

  // for global shm to do acl
  meta::Uid user{0};
  // for global shm to be freed after owning process is gone
  int pid{0};
  int ppid{0};
  Uuid id;

  // for fuse
  std::string key;
  int iorIndex = -1;
  bool isIoRing = false;
  bool forRead = true;
  int ioDepth = 0;
  std::optional<IorAttrs> iora;

 private:
  void mapBuf();

 private:
  bool owner_;
  int numaNode_;
  //  int fd_;

  // for client agent
  std::vector<folly::atomic_shared_ptr<storage::client::IOBuffer>> memhs_;
  folly::coro::Baton memhBaton_;
  std::atomic<bool> regging_;
};

class ShmBufForIO {
 public:
  ShmBufForIO(std::shared_ptr<ShmBuf> buf, off_t off)
      : buf_(std::move(buf)),
        off_(off) {}
  uint8_t *ptr() const {
    XLOGF(DBG, "buf start {} off {} ptr {}", (void *)buf_->bufStart, off_, (void *)(buf_->bufStart + off_));
    return buf_->bufStart + off_;
  }
  CoTryTask<storage::client::IOBuffer *> memh(size_t len) const {
    XLOGF(DBG, "shm buf for io off {} buf ptr {}", off_, (void *)buf_.get());
    XLOGF(DBG, "shm block size {}", buf_->blockSize);
    if (len && off_ / buf_->blockSize != (off_ + len - 1) / buf_->blockSize) {
      co_return makeError(StatusCode::kInvalidArg);
    }
    co_return (co_await buf_->memh(off_)).get();
  }

 private:
  std::shared_ptr<ShmBuf> buf_;
  off_t off_;
};

using ProcShmBuf = AllProcMap<ShmBuf, StatusCode::kInvalidArg, StatusCode::kNotEnoughMemory>;
}  // namespace hf3fs::lib
