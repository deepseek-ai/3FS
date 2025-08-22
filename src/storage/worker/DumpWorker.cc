#include "storage/worker/DumpWorker.h"

#include <gperftools/profiler.h>
#include <fstream>
#include <memory>
#include <sys/times.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {
namespace {

monitor::ValueRecorder cpuCores{"storage.sys.cpu_cores", std::nullopt, true};

}

DumpWorker::DumpWorker(const Config &config, Components &components)
    : config_(config),
      components_(components),
      executors_(std::make_pair(1u, 1u), std::make_shared<folly::NamedThreadFactory>("Dump")) {}

// start dump worker.
Result<Void> DumpWorker::start(flat::NodeId id) {
  executors_.add([this] { loop(); });
  started_ = true;
  nodeId_ = id;
  return Void{};
}

// stop dump worker. End all tasks immediately.
Result<Void> DumpWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for DumpWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  executors_.join();
  return Void{};
}

void DumpWorker::loop() {
  RelativeTime lastDumpTime = RelativeTime::now();

  struct tms last_tms {};
  struct tms cur_tms {};
  auto last_tck = times(&last_tms);

  bool profiler = false;
  RelativeTime lastProfilerTime = RelativeTime::now();

  while (!stopping_) {
    auto lock = std::unique_lock(mutex_);
    if (cond_.wait_for(lock, 1000_ms, [&] { return stopping_.load(); })) {
      break;
    }

    auto cur_tck = times(&cur_tms);
    if (last_tck < cur_tck && last_tms.tms_stime <= cur_tms.tms_stime && last_tms.tms_utime <= cur_tms.tms_utime) {
      auto elapsed = cur_tck - last_tck;
      auto usage = (cur_tms.tms_stime - last_tms.tms_stime) + (cur_tms.tms_utime - last_tms.tms_utime);
      auto cores = usage / elapsed;
      cpuCores.set(cores);
      if (!profiler && cores >= config_.high_cpu_usage_threshold()) {
        profiler = true;
        lastProfilerTime = RelativeTime::now();
        profilerStart(config_.dump_root_path());
      }
    }
    last_tck = cur_tck;
    last_tms = cur_tms;

    if (profiler && RelativeTime::now() - lastProfilerTime >= 1_min) {
      ProfilerStop();
      profiler = false;
    }

    // 1. dump all targets.
    auto now = RelativeTime::now();
    if (now - lastDumpTime >= config_.dump_interval()) {
      auto rootPath = config_.dump_root_path();
      if (rootPath.empty()) {
        continue;
      }

      dump(rootPath);
      lastDumpTime = now;
      last_tck = times(&last_tms);
    }
  }
  stopped_ = true;
  XLOGF(INFO, "DumpWorker@{}::loop stopped", fmt::ptr(this));
}

Result<Void> DumpWorker::dump(const Path &rootPath) {
  auto hostname = SysResource::hostname().value_or("unknown");

  auto dumpPath = rootPath / fmt::format("{:%F}", fmt::localtime(std::time(nullptr)));
  boost::system::error_code ec{};
  boost::filesystem::create_directories(dumpPath, ec);
  if (UNLIKELY(ec.failed())) {
    auto msg = fmt::format("dump meta create directory {} failed: {}", dumpPath, ec.message());
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }

  std::map<std::string, std::weak_ptr<StorageTarget>> targets;
  {
    auto targetMap = components_.targetMap.snapshot();
    for (auto &[targetId, target] : targetMap->getTargets()) {
      if (target.localState != flat::LocalTargetState::OFFLINE && target.storageTarget != nullptr) {
        auto dumpFileName = fmt::format("{}.{}.{}.{}",
                                        target.vChainId.chainId.toUnderType(),
                                        target.vChainId.chainVer.toUnderType(),
                                        targetId.toUnderType(),
                                        hostname);
        targets[dumpFileName] = target.storageTarget;
      }
    }
  }

  for (auto &[name, weakTarget] : targets) {
    if (stopping_) {
      break;
    }
    auto target = weakTarget.lock();
    if (!target) {
      continue;
    }

    auto dumpFilePath = dumpPath / name;
    std::unordered_map<ChunkId, ChunkMetadata> metas;
    auto dumpResult = target->getAllMetadataMap(metas);
    if (UNLIKELY(!dumpResult)) {
      XLOGF(ERR, "dump meta {} failed: {}", dumpFilePath, dumpResult.error());
      return makeError(std::move(dumpResult.error()));
    }
    target = nullptr;

    std::ofstream dumpFile{dumpFilePath};
    if (UNLIKELY(!dumpFile)) {
      auto msg = fmt::format("dump meta create file failed: {}", dumpFilePath);
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageInitFailed, std::move(msg));
    }
    auto bytes = serde::serializeBytes(metas);
    auto view = std::string_view{bytes};
    dumpFile.write(view.data(), view.size());
    if (UNLIKELY(!dumpFile)) {
      auto msg = fmt::format("dump meta write file failed: {}", dumpFilePath);
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageInitFailed, std::move(msg));
    }
  }
  return Void{};
}

Result<Void> DumpWorker::profilerStart(const Path &rootPath) {
  auto dumpPath = rootPath / fmt::format("{:%F}", fmt::localtime(std::time(nullptr)));
  boost::system::error_code ec{};
  boost::filesystem::create_directories(dumpPath, ec);
  if (UNLIKELY(ec.failed())) {
    auto msg = fmt::format("dump meta create directory {} failed: {}", dumpPath, ec.message());
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }

  auto dumpFile = dumpPath / fmt::format("{}.{:%T}.perf", nodeId_.toUnderType(), fmt::localtime(std::time(nullptr)));
  ProfilerStart(dumpFile.c_str());

  return Void{};
}

}  // namespace hf3fs::storage
