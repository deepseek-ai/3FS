#include "storage/update/UpdateWorker.h"

namespace hf3fs::storage {

Result<Void> UpdateWorker::start(uint32_t numberOfDisks) {
  if (config_.num_threads() < numberOfDisks) {
    return makeError(StatusCode::kInvalidConfig,
                     fmt::format("too few update worker threads, {} < {}", config_.num_threads(), numberOfDisks));
  }

  queueVec_.reserve(numberOfDisks);
  for (auto i = 0u; i < numberOfDisks; ++i) {
    queueVec_.emplace_back(std::make_unique<Queue>(config_.queue_size()));
  }

  for (auto i = 0u; i < config_.num_threads(); ++i) {
    executors_.add([this, i] { run(*queueVec_[i % queueVec_.size()]); });
  }
  return Void{};
}

void UpdateWorker::stopAndJoin() {
  if (stopped_.test_and_set()) {
    return;
  }

  for (auto i = 0u; i < config_.num_threads() && !queueVec_.empty(); ++i) {
    queueVec_[i % queueVec_.size()]->enqueue(nullptr);
  }
  executors_.join();
  bgExecutors_.join();
}

void UpdateWorker::run(Queue &queue) {
  while (true) {
    auto job = queue.dequeue();
    if (UNLIKELY(job == nullptr)) {
      XLOGF(DBG, "Storage worker {} stop...", fmt::ptr(this));
      break;
    }

    job->target()->updateChunk(*job, bgExecutors_);
  }
}

}  // namespace hf3fs::storage
