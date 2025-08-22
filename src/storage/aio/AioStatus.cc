#include "storage/aio/AioStatus.h"

#include <chrono>
#include <liburing.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "storage/aio/BatchReadJob.h"

namespace hf3fs::storage {
namespace {

monitor::DistributionRecorder inflightNum("storage.aio.inflight");
monitor::CountRecorder aioReadFailCount{"storage.aio.fail_count"};
monitor::CountRecorder aioReadEIOCount{"storage.aio.eio_count"};

monitor::OperationRecorder ioCollectRecorder{"storage.io_collect"};

monitor::OperationRecorder ioSubmitRecorder{"storage.io_submit"};
monitor::DistributionRecorder ioSubmitSize("storage.io_submit.size");
monitor::DistributionRecorder ioSubmitLoop("storage.io_submit.loop");
monitor::CountRecorder ioSubmitBadFd("storage.io_submit.badfd_count");
monitor::CountRecorder ioSubmitError("storage.io_submit.error_count");
monitor::OperationRecorder ioGetEventsRecorder{"storage.io_getevents"};
monitor::DistributionRecorder ioGetEventsSize("storage.io_getevents.size");

void setReadJobResult(void *raw, int64_t res) {
  auto job = reinterpret_cast<AioReadJob *>(raw);
  auto storageTarget = job->state().storageTarget;
  if (res >= 0) {
    auto latency = RelativeTime::now() - job->startTime();
    storageTarget->recordRealRead(res, latency);
    auto length = std::min(std::min(std::max(0l, res - job->state().headLength), int64_t(job->readIO().length)),
                           std::max(0l, int64_t(job->state().chunkLen) - job->readIO().offset));
    if (UNLIKELY(length == 0 && job->readIO().length > 0)) {
      XLOGF(WARNING, "read length is 0: {}, state: {}", job->readIO(), job->state());
    }
    job->setResult(length);
    // WARNING: job is no longer available.
  } else {
    if (storageTarget == nullptr) {
      aioReadFailCount.addSample(1);
    } else {
      aioReadFailCount.addSample(1, storageTarget->tag());
    }
    XLOGF(ERR,
          "set read job failed: {}, state: {}, buf: {}, code: {}",
          job->readIO(),
          job->state(),
          fmt::ptr(job->state().localbuf.ptr()),
          -res);
    job->setResult(makeError(StorageCode::kChunkReadFailed, fmt::format("errno: {}", -res)));
    // WARNING: job is no longer available.
  }
}

}  // namespace

AioStatus::~AioStatus() {
  if (aioContext_) {
    ::io_destroy(aioContext_);
  }
}

Result<Void> AioStatus::init(uint32_t maxEvents) {
  maxEvents_ = maxEvents;

  // 1. init aio context.
  int ret = ::io_setup(maxEvents, &aioContext_);
  if (UNLIKELY(ret != 0)) {
    auto msg = fmt::format("init aio context failed: {}, maxEvents {}", ret, maxEvents);
    XLOG(ERR, msg);
    return makeError(StatusCode::kInvalidConfig, std::move(msg));
  }

  // 2. init iocb.
  iocbs_.resize(maxEvents);
  availables_.reserve(maxEvents);
  for (auto &iocb : iocbs_) {
    availables_.push_back(&iocb);
  }
  events_.resize(maxEvents);
  return Void{};
}

void AioStatus::collect() {
  auto recordGuard = ioCollectRecorder.record();
  while (availableToSubmit() && iterator_) {
    auto &job = *iterator_++;
    auto result = job.state().storageTarget->aioPrepareRead(job);
    if (UNLIKELY(!result)) {
      job.setResult(makeError(std::move(result.error())));
      continue;
    }

    ++readyToSubmit_;
    ++inflight_;
    auto iocb = availables_.back();
    availables_.pop_back();
    auto &state = job.state();
    job.resetStartTime();
    ::io_prep_pread(iocb, state.readFd, state.localbuf.ptr(), state.readLength, state.readOffset);
    iocb->data = &job;
  }
  recordGuard.succ();
}

void AioStatus::submit() {
  uint32_t submitStartPoint = availables_.size();
  uint32_t loopCnt = 0;
  while (readyToSubmit_) {
    ++loopCnt;
    auto recordGuard = ioSubmitRecorder.record();
    int ret = ::io_submit(aioContext_, readyToSubmit_, &availables_[submitStartPoint]);
    auto elapsedTime = RelativeTime::now() - recordGuard.startTime();
    if (UNLIKELY(elapsedTime >= 5_s)) {
      XLOGF(WARNING, "io_submit took too long {}, submit {} ret {}", elapsedTime.asMs(), readyToSubmit_, ret);
    }
    if (ret >= 0) {
      recordGuard.succ();
      ioSubmitSize.addSample(ret);
      submitStartPoint += ret;
      readyToSubmit_ -= ret;
    } else if (ret == -EAGAIN) {
      continue;
    } else if (ret == -EBADF) {
      XLOGF(ERR, "aio submit bad file descriptor {}. ret: {}", availables_[submitStartPoint]->aio_fildes, ret);
      // set failed and skip it.
      ioSubmitBadFd.addSample(1);
      setReadJobResult(availables_[submitStartPoint]->data, -EBADF);
      availables_.push_back(availables_[submitStartPoint]);
      ++submitStartPoint;
      --readyToSubmit_;
      --inflight_;
    } else {
      ioSubmitError.addSample(1);
      XLOGF(ERR, "Unrecoverable aio submit error. ret: {}", ret);
      // set all jobs failed.
      while (readyToSubmit_) {
        setReadJobResult(availables_[submitStartPoint]->data, ret);
        availables_.push_back(availables_[submitStartPoint]);
        ++submitStartPoint;
        --readyToSubmit_;
        --inflight_;
      }
      break;
    }
  }
  ioSubmitLoop.addSample(loopCnt);
  inflightNum.addSample(inflight());
}

void AioStatus::reap(uint32_t minCompleteIn) {
  uint32_t minComplete = std::min(inflight(), minCompleteIn);
  auto recordGuard = ioGetEventsRecorder.record();
  int ret = ::io_getevents(aioContext_, minComplete, inflight(), events_.data(), nullptr);
  if (LIKELY(ret >= 0)) {
    recordGuard.succ();
    ioGetEventsSize.addSample(ret);
    inflight_ -= ret;
    for (int i = 0; i < ret; ++i) {
      auto &event = events_[i];
      availables_.push_back(event.obj);
      setReadJobResult(event.data, event.res);
    }
  } else if (ret == -EINTR) {
    XLOGF(INFO, "aio is interrupted by a signal handler");
    return;
  } else {
    XLOGF(ERR, "aio io_getevents error: {}", ret);
    return;
  }
}

IoUringStatus::~IoUringStatus() {
  if (ring_.ring_fd) {
    ::io_uring_queue_exit(&ring_);
  }
}

Result<Void> IoUringStatus::init(uint32_t maxEvents,
                                 const std::vector<int> &fds,
                                 const std::vector<struct iovec> &iovecs) {
  maxEvents_ = maxEvents;

  auto ret = ::io_uring_queue_init(maxEvents_, &ring_, 0);
  if (UNLIKELY(ret != 0)) {
    auto msg = fmt::format("init io uring failed: {}, maxEvents {}", ret, maxEvents);
    XLOG(ERR, msg);
    return makeError(StatusCode::kInvalidConfig, std::move(msg));
  }
  submittingJobs_.reserve(maxEvents_);

  if (!fds.empty()) {
    int ret = ::io_uring_register_files(&ring_, fds.data(), fds.size());
    if (UNLIKELY(ret != 0)) {
      auto msg = fmt::format("io_uring_register_files failed: {}, size: {}", ret, fds.size());
      XLOG(ERR, msg);
      return makeError(StatusCode::kInvalidConfig, std::move(msg));
    }
  }
  if (!iovecs.empty()) {
    int ret = ::io_uring_register_buffers(&ring_, iovecs.data(), iovecs.size());
    if (UNLIKELY(ret != 0)) {
      auto msg = fmt::format("io_uring_register_buffers failed: {}, size: {}", ret, iovecs.size());
      XLOG(ERR, msg);
      return makeError(StatusCode::kInvalidConfig, std::move(msg));
    }
  }

  return Void{};
}

void IoUringStatus::collect() {
  auto recordGuard = ioCollectRecorder.record();
  while (availableToSubmit() && iterator_) {
    auto &job = *iterator_++;
    auto result = job.state().storageTarget->aioPrepareRead(job);
    if (UNLIKELY(!result)) {
      job.setResult(makeError(std::move(result.error())));
      continue;
    }

    ++inflight_;
    auto &state = job.state();

    job.resetStartTime();
    struct io_uring_sqe *sqe = ::io_uring_get_sqe(&ring_);
    assert(sqe != nullptr);
    ::io_uring_prep_read_fixed(sqe,
                               state.fdIndex.value_or(state.readFd),
                               state.localbuf.ptr(),
                               state.readLength,
                               state.readOffset,
                               state.bufferIndex);
    if (state.fdIndex) {
      sqe->flags |= IOSQE_FIXED_FILE;
    }
    ::io_uring_sqe_set_data(sqe, &job);
    submittingJobs_.push_back(&job);
  }
  recordGuard.succ();
}

void IoUringStatus::submit() {
  auto recordGuard = ioSubmitRecorder.record();
  int ret = ::io_uring_submit(&ring_);
  if (LIKELY(ret >= 0)) {
    assert(ret == (int)inflight_);
    recordGuard.succ();
    ioSubmitSize.addSample(ret);
  } else {
    XLOGF(CRITICAL, "io_uring submit error: {}", ret);
    for (auto &job : submittingJobs_) {
      setReadJobResult(job, ret);
    }
    inflight_ -= submittingJobs_.size();
  }
  submittingJobs_.clear();
}

void IoUringStatus::reap(uint32_t minCompleteIn) {
  auto recordGuard = ioGetEventsRecorder.record();
  io_uring_cqe *cqe = nullptr;
  int ret = ::io_uring_wait_cqes(&ring_, &cqe, std::min(inflight(), minCompleteIn), nullptr, nullptr);
  if (LIKELY(ret >= 0)) {
    recordGuard.succ();
    uint32_t cnt = 0;
    unsigned head = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      ++cnt;
      setReadJobResult(::io_uring_cqe_get_data(cqe), cqe->res);
    }
    ioGetEventsSize.addSample(cnt);
    inflight_ -= cnt;
    ::io_uring_cq_advance(&ring_, cnt);
  } else if (ret == -EINTR) {
    XLOGF(INFO, "io_uring is interrupted by a signal handler");
    return;
  } else {
    XLOGF(ERR, "io_uring wait_cqes error: {}", ret);
    return;
  }
}

}  // namespace hf3fs::storage
