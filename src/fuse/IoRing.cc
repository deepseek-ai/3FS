#include "IoRing.h"

#include <optional>
#include <type_traits>
#include <utility>

#include "PioV.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Schema.h"
#include "fuse/FuseClients.h"
#include "fuse/FuseOps.h"
#include "lib/api/hf3fs_usrbio.h"

namespace hf3fs::fuse {
std::vector<IoRingJob> IoRing::jobsToProc(int maxJobs) {
  std::vector<IoRingJob> jobs;

  std::lock_guard lock(cqeMtx_);
  auto spt = sqeProcTail_;
  auto sqes = sqeCount();

  auto cqeAvail = entries - 1 - processing_ - cqeCount();
  while (sqes && (int)jobs.size() < maxJobs) {
    int toProc;
    if (ioDepth > 0) {
      toProc = ioDepth;
      if (toProc > sqes || toProc > cqeAvail) {  // even if we finish the io, we got no place to store the results
        break;
      }
    } else {
      toProc = std::min(sqes, cqeAvail);
      if (ioDepth < 0) {
        auto iod = -ioDepth;
        if (toProc > iod) {
          toProc = iod;
        } else if (toProc < iod && timeout.count()) {
          auto now = lastCheck_ = SteadyClock::now();
          if (!lastCheck_) {  // first time to find the (not enough) ios, wait till timeout
            lastCheck_ = now;
            break;
          } else if (*lastCheck_ + timeout > now) {  // ios not enough to fill a batch, and time has not run out
            break;
          }
        }

        lastCheck_ = std::nullopt;
      }
    }

    if (jobs.empty()) {
      jobs.reserve(ioDepth ? std::min(maxJobs, sqes / abs(ioDepth) + 1) : 1);
    }

    jobs.emplace_back(IoRingJob{shared_from_this(), spt, toProc});

    spt = (spt + toProc) % entries;
    sqeProcTails_.push_back(spt);
    processing_ += toProc;
    sqes -= toProc;
    cqeAvail -= toProc;
  }

  sqeProcTail_ = spt;
  return jobs;
}

CoTask<void> IoRing::process(
    int spt,
    int toProc,
    storage::client::StorageClient &storageClient,
    const storage::client::IoOptions &storageIo,
    UserConfig &userConfig,
    std::function<void(std::vector<std::shared_ptr<RcInode>> &, const IoArgs *, const IoSqe *, int)> &&lookupFiles,
    std::function<void(std::vector<Result<lib::ShmBufForIO>> &, const IoArgs *, const IoSqe *, int)> &&lookupBufs) {
  static monitor::LatencyRecorder overallLatency("usrbio.piov.overall", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder prepareLatency("usrbio.piov.prepare", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder submitLatency("usrbio.piov.submit", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder completeLatency("usrbio.piov.complete", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ioSizeDist("usrbio.piov.io_size", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ioDepthDist("usrbio.piov.io_depth", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder totalBytesDist("usrbio.piov.total_bytes",
                                                      monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder distinctFilesDist("usrbio.piov.distinct_files",
                                                         monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder distinctBufsDist("usrbio.piov.distinct_bufs",
                                                        monitor::TagSet{{"mount_name", mountName}});
  static monitor::CountRecorder bwCount("usrbio.piov.bw", monitor::TagSet{{"mount_name", mountName}});

  auto start = SteadyClock::now(), overallStart = start;
  std::string ioType = forRead_ ? "read" : "write";
  auto uids = std::to_string(userInfo_.uid.toUnderType());

  auto &config = userConfig.getConfig(userInfo_);

  std::vector<ssize_t> res;
  if (!forRead_ && config.readonly()) {
    res = std::vector<ssize_t>(toProc, static_cast<ssize_t>(-StatusCode::kReadOnlyMode));
  } else {
    res = std::vector<ssize_t>(toProc, 0);

    size_t iod = 0, totalBytes = 0;
    std::set<uint64_t> distinctFiles;
    std::set<Uuid> distinctBufs;

    std::vector<std::shared_ptr<RcInode>> inodes;
    inodes.reserve(toProc);
    lookupFiles(inodes, ringSection, sqeSection + spt, std::min(toProc, entries - spt));
    if ((int)inodes.size() < toProc) {
      lookupFiles(inodes, ringSection, sqeSection, toProc - (int)inodes.size());
    }

    std::vector<Result<lib::ShmBufForIO>> bufs;
    bufs.reserve(toProc);
    lookupBufs(bufs, ringSection, sqeSection + spt, std::min(toProc, entries - spt));
    if ((int)bufs.size() < toProc) {
      lookupBufs(bufs, ringSection, sqeSection, toProc - (int)bufs.size());
    }

    lib::agent::PioV ioExec(storageClient, config.chunk_size_limit(), res);
    std::vector<uint64_t> truncateVers;
    if (!forRead_) {
      truncateVers.resize(toProc, 0);
    }

    for (int i = 0; i < toProc; ++i) {
      auto idx = (spt + i) % entries;
      auto sqe = sqeSection[idx];

      const auto &args = ringSection[sqe.index];

      ++iod;
      totalBytes += args.ioLen;
      distinctFiles.insert(args.fileIid);

      Uuid id;
      memcpy(id.data, args.bufId, sizeof(id.data));
      distinctBufs.insert(id);

      ioSizeDist.addSample(args.ioLen, monitor::TagSet{{"io", ioType}, {"uid", uids}});

      if (!inodes[i]) {
        res[i] = -static_cast<ssize_t>(MetaCode::kNotFile);
        continue;
      }

      if (!bufs[i]) {
        res[i] = -static_cast<ssize_t>(bufs[i].error().code());
        continue;
      }

      auto memh = co_await bufs[i]->memh(args.ioLen);
      if (!memh) {
        res[i] = -static_cast<ssize_t>(memh.error().code());
        continue;
      } else if (!bufs[i]->ptr() || !*memh) {
        XLOGF(ERR, "{} is null when doing usrbio", *memh ? "buf ptr" : "memh");
        res[i] = -static_cast<ssize_t>(ClientAgentCode::kIovShmFail);
        continue;
      }

      if (!forRead_) {
        auto beginWrite =
            co_await inodes[i]->beginWrite(userInfo_, *getFuseClientsInstance().metaClient, args.fileOff, args.ioLen);
        if (beginWrite.hasError()) {
          res[i] = -static_cast<ssize_t>(beginWrite.error().code());
          continue;
        }
        truncateVers[i] = *beginWrite;
      }

      auto addRes = forRead_
                        ? ioExec.addRead(i, inodes[i]->inode, 0, args.fileOff, args.ioLen, bufs[i]->ptr(), **memh)
                        : ioExec.addWrite(i, inodes[i]->inode, 0, args.fileOff, args.ioLen, bufs[i]->ptr(), **memh);
      if (!addRes) {
        res[i] = -static_cast<ssize_t>(addRes.error().code());
      }
    }

    auto now = SteadyClock::now();
    prepareLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    start = now;

    ioDepthDist.addSample(iod, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    totalBytesDist.addSample(totalBytes, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    distinctFilesDist.addSample(distinctFiles.size(), monitor::TagSet{{"io", ioType}, {"uid", uids}});
    distinctBufsDist.addSample(distinctBufs.size(), monitor::TagSet{{"io", ioType}, {"uid", uids}});

    auto readOpt = storageIo.read();
    if (flags_ & HF3FS_IOR_ALLOW_READ_UNCOMMITTED) {
      readOpt.set_allowReadUncommitted(true);
    }
    auto execRes = co_await (forRead_ ? ioExec.executeRead(userInfo_, readOpt)
                                      : ioExec.executeWrite(userInfo_, storageIo.write()));

    now = SteadyClock::now();
    submitLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    start = now;

    if (!execRes) {
      for (auto &r : res) {
        if (r >= 0) {
          r = -static_cast<ssize_t>(execRes.error().code());
        }
      }
    } else {
      ioExec.finishIo(!(flags_ & HF3FS_IOR_FORBID_READ_HOLES));
    }

    if (!forRead_) {
      for (int i = 0; i < toProc; ++i) {
        auto &inode = inodes[i];
        if (!inode) {
          continue;
        }
        auto sqe = sqeSection[(spt + i) % entries];
        auto off = ringSection[sqe.index].fileOff;
        auto r = res[i];
        inode->finishWrite(userInfo_.uid, truncateVers[i], off, r);
      }
    }
  }

  auto newSpt = (spt + toProc) % entries;

  std::vector<IoSqe> sqes(toProc);
  for (int i = 0; i < toProc; ++i) {
    sqes[i] = sqeSection[(spt + i) % entries];
  }

  {
    // lock for between threads (io workers)
    // atomics for between processes (io worker & io generator)
    std::lock_guard lock(cqeMtx_);
    if (sqeProcTails_.empty()) {
      XLOGF(FATAL, "bug?! sqeProcTails_ is empty");
    }

    if (sqeProcTails_.front() != newSpt) {
      sqeDoneTails_.insert(newSpt);
    } else {
      sqeTail = newSpt;
      sqeProcTails_.pop_front();
      while (!sqeDoneTails_.empty()) {
        if (sqeProcTails_.empty()) {
          XLOGF(FATAL, "bug?! sqeProcTails_ is empty");
        }
        auto first = sqeProcTails_.front();
        auto it = sqeDoneTails_.find(first);
        if (it == sqeDoneTails_.end()) {
          break;
        } else {
          sqeTail = first;
          sqeProcTails_.pop_front();
          sqeDoneTails_.erase(it);
        }
      }
    }

    for (int i = 0; i < toProc; ++i) {
      auto &sqe = sqes[i];
      auto r = res[i];
      auto addRes = addCqe(sqe.index, r >= 0 ? r : -static_cast<ssize_t>(StatusCode::toErrno(-r)), sqe.userdata);
      if (!addRes) {
        XLOGF(FATAL, "failed to add cqe");
      }
    }

    processing_ -= toProc;
  }

  sem_post(cqeSem.get());

  size_t doneBytes = 0;
  for (auto r : res) {
    if (r > 0) {
      doneBytes += r;
    }
  }
  bwCount.addSample(doneBytes, monitor::TagSet{{"io", ioType}, {"uid", uids}});

  auto now = SteadyClock::now();
  completeLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
  overallLatency.addSample(now - overallStart, monitor::TagSet{{"io", ioType}, {"uid", uids}});
}
}  // namespace hf3fs::fuse
