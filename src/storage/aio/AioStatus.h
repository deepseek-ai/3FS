#pragma once

#include <libaio.h>
#include <liburing.h>
#include <vector>

#include "storage/aio/BatchReadJob.h"
#include "storage/store/StorageTargets.h"

namespace hf3fs::storage {

class IoStatus {
 public:
  virtual ~IoStatus() = default;

  bool hasUnfinishedBatchReadJob() const { return iterator_; }

  void setAioReadJobIterator(AioReadJobIterator it) { iterator_ = it; }

  bool availableToSubmit() const { return inflight_ < maxEvents_; }

  uint32_t inflight() const { return inflight_; }

  virtual void collect() = 0;

  virtual void submit() = 0;

  virtual void reap(uint32_t minCompleteIn) = 0;

 protected:
  AioReadJobIterator iterator_;
  uint32_t maxEvents_ = 0;
  uint32_t inflight_ = 0;
};

class AioStatus : public IoStatus {
 public:
  ~AioStatus() override;

  Result<Void> init(uint32_t maxEvents);

  void collect() override;

  void submit() override;

  void reap(uint32_t minCompleteIn) override;

 private:
  uint32_t readyToSubmit_ = 0;
  io_context_t aioContext_ = nullptr;
  std::vector<struct iocb> iocbs_;
  std::vector<struct iocb *> availables_;
  std::vector<struct io_event> events_;
};

class IoUringStatus : public IoStatus {
 public:
  ~IoUringStatus() override;

  Result<Void> init(uint32_t maxEvents, const std::vector<int> &fds, const std::vector<struct iovec> &iovecs);

  void collect() override;

  void submit() override;

  void reap(uint32_t minCompleteIn) override;

 private:
  struct io_uring ring_ {};
  std::vector<AioReadJob *> submittingJobs_;
};

}  // namespace hf3fs::storage
