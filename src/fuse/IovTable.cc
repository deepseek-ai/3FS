#include "IovTable.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <folly/experimental/coro/BlockingWait.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "IoRing.h"
#include "fbs/meta/Common.h"
#ifdef HF3FS_GDR_ENABLED
#include "common/net/ib/AcceleratorMemory.h"
#endif
#include "lib/common/GdrUri.h"

namespace hf3fs::fuse {

using hf3fs::lib::IorAttrs;

const Path linkPref = "/dev/shm";

namespace {

std::optional<size_t> parsePositiveSize(std::string_view text) {
  if (text.empty()) return std::nullopt;
  size_t value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> parseNonNegativeInt(std::string_view text) {
  if (text.empty()) return std::nullopt;
  int value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0) {
    return std::nullopt;
  }
  return value;
}

std::optional<uint64_t> parseBinaryFlags(std::string_view text) {
  if (text.empty()) return std::nullopt;
  uint64_t value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 2);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

void IovTable::init(const Path &mount, int cap) {
  mountName = mount.native();
  iovs = std::make_unique<AtomicSharedPtrTable<lib::ShmBuf>>(cap);
}

struct IovAttrs {
  Uuid id;
  size_t blockSize = 0;
  bool isIoRing = false;
  bool forRead = true;
  int ioDepth = 0;
  std::optional<IorAttrs> iora;
  bool isGdr = false;
  int gpuDeviceId = -1;
};

static Result<IovAttrs> parseKey(const char *key) {
  IovAttrs iova;

  std::vector<std::string> fnParts;
  folly::split('.', key, fnParts);
  if (fnParts.empty() || fnParts[0].empty()) {
    return makeError(StatusCode::kInvalidArg, "invalid shm key");
  }

  auto idRes = Uuid::fromHexString(fnParts[0]);
  RETURN_ON_ERROR(idRes);
  iova.id = *idRes;

  for (size_t i = 1; i < fnParts.size(); ++i) {
    auto dec = fnParts[i];
    if (dec.empty()) {
      return makeError(StatusCode::kInvalidArg, "empty attr in shm key");
    }
    switch (dec[0]) {
      case 'b': {  // block size
        auto blockSize = parsePositiveSize(std::string_view(dec).substr(1));
        if (!blockSize) {
          return makeError(StatusCode::kInvalidArg, "invalid block size set in shm key");
        }
        iova.blockSize = *blockSize;
        break;
      }

      case 'r':
      case 'w': {  // is io ring
        auto depth = parseNonNegativeInt(std::string_view(dec).substr(1));
        if (!depth) {
          return makeError(StatusCode::kInvalidArg, "invalid io batch size set in shm key");
        }
        iova.isIoRing = true;
        iova.forRead = dec[0] == 'r';
        iova.ioDepth = *depth;
        break;
      }

      case 't': {
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        auto timeoutMs = parseNonNegativeInt(std::string_view(dec).substr(1));
        if (!timeoutMs) {
          return makeError(StatusCode::kInvalidArg, "invalid io job check timeout {}", dec.c_str() + 1);
        }
        iova.iora->timeout = Duration(std::chrono::nanoseconds((uint64_t)*timeoutMs * 1000000));
        break;
      }

      case 'f': {
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        auto flags = parseBinaryFlags(std::string_view(dec).substr(1));
        if (!flags) {
          return makeError(StatusCode::kInvalidArg, "invalid io exec flags {}", dec.c_str() + 1);
        }
        iova.iora->flags = *flags;
        break;
      }

      case 'p':  // should be io ring, priority
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        if (dec.size() > 2) {
          return makeError(StatusCode::kInvalidArg, "invalid priority set in shm key");
        }
        switch (dec.c_str()[1]) {
          case 'l':
            iova.iora->priority = 2;
            break;
          case 'h':
            iova.iora->priority = 0;
            break;
          case 'n':
          case '\0':
            iova.iora->priority = 1;
            break;
          default:
            return makeError(StatusCode::kInvalidArg, "invalid priority set in shm key");
        }
        break;

      case 'g':  // gdr marker (e.g. ".gdr")
        if (dec == "gdr") {
          iova.isGdr = true;
        } else {
          return makeError(StatusCode::kInvalidArg, "invalid gdr attr in shm key");
        }
        break;

      case 'd': {  // gpu device id (e.g. ".d0", ".d1")
        auto devId = parseNonNegativeInt(std::string_view(dec).substr(1));
        if (!devId) {
          return makeError(StatusCode::kInvalidArg, "invalid gpu device id in key");
        }
        iova.gpuDeviceId = *devId;
        break;
      }
      default:
        return makeError(StatusCode::kInvalidArg, "unknown attr in shm key");
    }
  }

  if (iova.isGdr) {
    if (iova.gpuDeviceId < 0) {
      return makeError(StatusCode::kInvalidArg, "gdr key missing gpu device id");
    }
    if (iova.blockSize != 0 || iova.isIoRing || iova.iora) {
      return makeError(StatusCode::kInvalidArg, "gdr key does not support block or io-ring attrs");
    }
  } else {
    if (!iova.isIoRing && iova.iora) {
      return makeError(StatusCode::kInvalidArg, "ioring attrs set for non-ioring");
    }
    if (iova.gpuDeviceId >= 0) {
      return makeError(StatusCode::kInvalidArg, "gpu device id set for non-gdr key");
    }
  }

  return iova;
}

constexpr int iovIidStart = meta::InodeId::iovIidStart;

std::optional<int> IovTable::iovDesc(meta::InodeId iid) {
  auto iidn = (ssize_t)iid.u64();
  auto diid = (ssize_t)meta::InodeId::iovDir().u64();
  if (iidn >= 0 || iidn > diid - iovIidStart || iidn < diid - std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return diid - iidn - iovIidStart;
}

Result<std::pair<meta::Inode, std::shared_ptr<lib::ShmBuf>>> IovTable::addIov(const char *key,
                                                                              const Path &shmPath,
                                                                              pid_t pid,
                                                                              const meta::UserInfo &ui,
                                                                              folly::Executor::KeepAlive<> exec,
                                                                              storage::client::StorageClient &sc) {
  static monitor::DistributionRecorder mapTimesCount("fuse.iov.times", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder mapBytesDist("fuse.iov.bytes", monitor::TagSet{{"mount_name", mountName}});
  static monitor::CountRecorder shmSizeCount("fuse.iov.total_bytes", monitor::TagSet{{"mount_name", mountName}}, false);
  static monitor::LatencyRecorder allocLatency("fuse.iov.latency.map", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ibRegBytesDist("fuse.iov.bytes.ib_reg",
                                                      monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder ibRegLatency("fuse.iov.latency.ib_reg", monitor::TagSet{{"mount_name", mountName}});

  auto iovaRes = parseKey(key);
  RETURN_ON_ERROR(iovaRes);

#ifndef HF3FS_GDR_ENABLED
  if (iovaRes->isGdr) {
    return makeError(StatusCode::kInvalidArg, "GDR not enabled in this build");
  }
#endif

#ifdef HF3FS_GDR_ENABLED
  // GDR path: shmPath is a gdr:// URI, not a filesystem path
  if (iovaRes->isGdr) {
    auto gdrTarget = lib::parseGdrUri(shmPath.native());
    if (!gdrTarget) {
      return makeError(StatusCode::kInvalidArg, "failed to parse GDR target URI");
    }
    if (iovaRes->gpuDeviceId != gdrTarget->deviceId) {
      return makeError(StatusCode::kInvalidArg,
                       "gdr key device {} does not match URI device {}",
                       iovaRes->gpuDeviceId,
                       gdrTarget->deviceId);
    }

    // parseGdrUri success guarantees a valid IPC handle.
    lib::GpuIpcHandle ipcHandle;
    std::memcpy(ipcHandle.data, gdrTarget->ipcHandle.data(), lib::kGdrIpcHandleBytes);
    ipcHandle.valid = true;

    // Import the GPU memory via IPC handle
    auto gpuShm = std::make_shared<lib::GpuShmBuf>(ipcHandle, gdrTarget->size, gdrTarget->deviceId, iovaRes->id);

    if (!gpuShm->devicePtr) {
      return makeError(StatusCode::kInvalidArg, "failed to import GPU memory via IPC handle");
    }

    gpuShm->key = key;
    gpuShm->user = ui.uid;
    gpuShm->pid = pid;

    // Allocate iov descriptor slot
    auto iovdRes = iovs->alloc();
    if (!iovdRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many iovs allocated");
    }
    auto iovd = *iovdRes;
    bool dealloc = true;
    SCOPE_EXIT {
      if (dealloc) {
        iovs->dealloc(iovd);
      }
    };

    // We store a null ShmBuf in the slot (GPU buffers are tracked via gpuShmsById)
    // but we still need the slot for iov descriptor numbering
    iovs->table[iovd].store(nullptr);

    // Register GPU memory for RDMA I/O
    auto recordMetrics = []() {};
    folly::coro::blockingWait(gpuShm->registerForIO(exec, sc, std::move(recordMetrics)));
    auto memh = folly::coro::blockingWait(gpuShm->memh(0));
    if (!memh) {
      folly::coro::blockingWait(gpuShm->deregisterForIO());
      return makeError(StatusCode::kIOError, "failed to register GPU memory for RDMA");
    }

    {
      std::lock_guard lock(gpuShmLock);
      gpuShmsById[iovaRes->id] = gpuShm;
      gpuIovMetaByIovd[iovd] = GpuIovMeta{std::string(key), shmPath, ui.uid, ui.gid, pid};
    }

    {
      std::unique_lock lock(iovdLock_);
      iovds_[key] = iovd;
    }

    // For GPU iovs, we return the GDR URI as the symlink target
    auto inode =
        meta::Inode{meta::InodeId::iov(iovd),
                    meta::InodeData{meta::Symlink{shmPath}, meta::Acl{ui.uid, ui.gid, meta::Permission(0400)}}};

    dealloc = false;
    return std::make_pair(inode, std::shared_ptr<lib::ShmBuf>());
  }
#endif

  Path shmOpenPath("/");
  shmOpenPath /= shmPath.lexically_relative(linkPref);

  struct stat st;
  if (stat(shmPath.c_str(), &st) == -1 || !S_ISREG(st.st_mode)) {
    return makeError(StatusCode::kInvalidArg, "failed to stat shm path or it's not a regular file");
  }

  if (iovaRes->blockSize > (size_t)st.st_size) {
    return makeError(StatusCode::kInvalidArg, "invalid block size set in shm key");
  } else if (iovaRes->isIoRing && iovaRes->ioDepth > IoRing::ioRingEntries((size_t)st.st_size)) {
    return makeError(StatusCode::kInvalidArg, "invalid io batch size set in shm key");
  }

  while (true) {
    auto iovdRes = iovs->alloc();
    if (!iovdRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many iovs allocated");
    }
    auto iovd = *iovdRes;
    bool dealloc = true;
    SCOPE_EXIT {
      if (dealloc) {
        iovs->dealloc(iovd);
      }
    };

    auto start = SteadyClock::now();
    auto uids = std::to_string(ui.uid.toUnderType());

    std::shared_ptr<lib::ShmBuf> shm;
    try {
      shm.reset(new lib::ShmBuf(shmOpenPath, 0, st.st_size, iovaRes->blockSize, iovaRes->id),
                [uids,
                 &shmSizeCount = shmSizeCount,
                 &mapTimesCount = mapTimesCount,
                 &mapBytesDist = mapBytesDist,
                 &allocLatency = allocLatency,
                 &ibRegLatency = ibRegLatency](auto p) {
                  auto start = SteadyClock::now();
                  folly::coro::blockingWait(p->deregisterForIO());
                  auto now = SteadyClock::now();
                  ibRegLatency.addSample(now - start, monitor::TagSet{{"instance", "dereg"}, {"uid", uids}});

                  start = now;
                  p->unmapBuf();
                  allocLatency.addSample(SteadyClock::now() - start,
                                         monitor::TagSet{{"instance", "free"}, {"uid", uids}});

                  mapTimesCount.addSample(1, monitor::TagSet{{"instance", "free"}, {"uid", uids}});
                  mapBytesDist.addSample(p->size, monitor::TagSet{{"instance", "free"}, {"uid", uids}});
                  shmSizeCount.addSample(-p->size);

                  delete p;
                });
    } catch (const std::runtime_error &e) {
      return makeError(ClientAgentCode::kIovShmFail, std::string("failed to open/map shm for iov ") + e.what());
    }

    allocLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    mapTimesCount.addSample(1, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    mapBytesDist.addSample(shm->size, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    shmSizeCount.addSample(shm->size, monitor::TagSet{{"uid", uids}});

    shm->key = key;
    shm->user = ui.uid;
    shm->pid = pid;
    shm->isIoRing = iovaRes->isIoRing;
    shm->forRead = iovaRes->forRead;
    shm->ioDepth = iovaRes->ioDepth;
    shm->iora = iovaRes->iora;

    // the idx should be reserved by us
    iovs->table[iovd].store(shm);

    start = SteadyClock::now();
    auto recordMetrics = [blockSize = shm->blockSize, start, uids]() mutable {
      ibRegBytesDist.addSample(blockSize, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
      ibRegLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
    };

    if (!iovaRes->isIoRing) {  // io ring bufs don't need to be registered for ib io
      folly::coro::blockingWait(shm->registerForIO(exec, sc, recordMetrics));
    }

    {
      std::unique_lock lock(iovdLock_);
      iovds_[key] = iovd;
    }

    {
      std::unique_lock lock(shmLock);
      shmsById[iovaRes->id] = iovd;
    }

    auto statRes = statIov(iovd, ui);
    RETURN_ON_ERROR(statRes);

    dealloc = false;
    return std::make_pair(*statRes, iovaRes->isIoRing ? shm : std::shared_ptr<lib::ShmBuf>());
  }
}

Result<std::shared_ptr<lib::ShmBuf>> IovTable::rmIov(const char *key, const meta::UserInfo &ui) {
  auto res = lookupIov(key, ui);
  RETURN_ON_ERROR(res);

  {
    std::unique_lock lock(iovdLock_);
    iovds_.erase(key);
  }

  auto parseRes = parseKey(key);

#ifdef HF3FS_GDR_ENABLED
  if (parseRes && parseRes->isGdr) {
    // GPU iov: clean up from gpuShmsById and gpuIovMetaByIovd
    auto iovd = iovDesc(res->id);
    {
      std::lock_guard lock(gpuShmLock);
      gpuShmsById.erase(parseRes->id);
      if (iovd) {
        gpuIovMetaByIovd.erase(*iovd);
      }
    }

    if (iovd) {
      // Must use dealloc() directly — not remove().
      // GPU slots store nullptr in iovs->table, and remove() early-returns
      // on null slots without calling dealloc(), leaking the descriptor.
      iovs->dealloc(*iovd);
    }

    return std::shared_ptr<lib::ShmBuf>();
  }
#endif

  {
    std::unique_lock lock(shmLock);
    if (parseRes) {
      shmsById.erase(parseRes->id);
    }
  }

  auto iovd = iovDesc(res->id);
  auto shm = iovs->table[*iovd].load();
  iovs->remove(*iovd);

  return shm;
}

Result<meta::Inode> IovTable::statIov(int iovd, const meta::UserInfo &ui) {
  if (iovd < 0 || iovd >= (int)iovs->table.size()) {
    return makeError(MetaCode::kNotFound, "invalid iov desc");
  }

  auto shm = iovs->table[iovd].load();
  if (!shm) {
#ifdef HF3FS_GDR_ENABLED
    // Check if this is a GPU iov
    std::lock_guard lock(gpuShmLock);
    auto git = gpuIovMetaByIovd.find(iovd);
    if (git != gpuIovMetaByIovd.end()) {
      auto &meta = git->second;
      if (meta.user != ui.uid) {
        XLOGF(ERR, "statting user {} gpu iov belongs to {}", ui.uid, meta.user);
        return makeError(MetaCode::kNoPermission, "iov not for user");
      }
      return meta::Inode{
          meta::InodeId::iov(iovd),
          meta::InodeData{meta::Symlink{meta.target}, meta::Acl{ui.uid, meta.gid, meta::Permission(0400)}}};
    }
#endif
    return makeError(MetaCode::kNotFound,
                     fmt::format("iov desc {} not found, next avail {}", iovd, iovs->slots.nextAvail.load()));
  }

  if (shm->user != ui.uid) {
    XLOGF(ERR, "statting user {} iov belongs to {}", ui.uid, shm->user);
    return makeError(MetaCode::kNoPermission, "iov not for user");
  }

  return meta::Inode{
      meta::InodeId::iov(iovd),
      meta::InodeData{meta::Symlink{linkPref / shm->path}, meta::Acl{ui.uid, ui.gid, meta::Permission(0400)}}};
}

Result<meta::Inode> IovTable::lookupIov(const char *key, const meta::UserInfo &ui) {
  int iovd = -1;
  {
    std::shared_lock lock(iovdLock_);
    auto it = iovds_.find(key);
    if (it == iovds_.end()) {
      return makeError(MetaCode::kNotFound, std::string("iov key not found ") + key);
    } else {
      iovd = it->second;
    }
  }

  return statIov(iovd, ui);
}

std::vector<int> IovTable::removeIovsByPid(pid_t pid) {
  struct IovToRemove {
    std::string key;
    meta::UserInfo ui;
    std::optional<int> ioRingIndex;
  };
#ifdef HF3FS_GDR_ENABLED
  struct GpuIovToRemove {
    int iovd;
    GpuIovMeta meta;
  };
#endif

  std::vector<IovToRemove> targets;
  std::vector<int> ioRingIndexes;
#ifdef HF3FS_GDR_ENABLED
  std::vector<GpuIovToRemove> gpuTargets;
#endif

  auto n = iovs->slots.nextAvail.load();
  targets.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto iov = iovs->table[i].load();
    if (!iov || iov->pid != pid) {
      continue;
    }
    targets.push_back(IovToRemove{iov->key,
                                  meta::UserInfo{iov->user, meta::Gid{iov->user.toUnderType()}},
                                  iov->isIoRing ? std::optional<int>{iov->iorIndex} : std::nullopt});
  }

#ifdef HF3FS_GDR_ENABLED
  {
    std::lock_guard lock(gpuShmLock);
    for (const auto &[iovd, meta] : gpuIovMetaByIovd) {
      if (meta.pid != pid) {
        continue;
      }
      gpuTargets.push_back(GpuIovToRemove{iovd, meta});
    }
  }
#endif

  ioRingIndexes.reserve(targets.size());
  for (const auto &target : targets) {
    XLOGF(INFO, "unlinking iov {} symlink from dead pid {}", target.key, pid);
    auto removeResult = rmIov(target.key.c_str(), target.ui);
    if (!removeResult) {
      XLOGF(WARN, "failed to unlink iov {} from dead pid {}: {}", target.key, pid, removeResult.error());
    }
    if (target.ioRingIndex) {
      ioRingIndexes.push_back(*target.ioRingIndex);
    }
  }

#ifdef HF3FS_GDR_ENABLED
  for (const auto &target : gpuTargets) {
    XLOGF(INFO, "unlinking gpu iov {} symlink from dead pid {}", target.meta.key, pid);
    auto parseResult = parseKey(target.meta.key.c_str());
    if (!parseResult) {
      XLOGF(WARN, "failed to parse gpu iov key {} from dead pid cleanup: {}", target.meta.key, parseResult.error());
      continue;
    }

    bool removed = false;
    {
      std::unique_lock iovdLock(iovdLock_);
      std::lock_guard lock(gpuShmLock);
      auto metaIt = gpuIovMetaByIovd.find(target.iovd);
      if (metaIt == gpuIovMetaByIovd.end() || metaIt->second.key != target.meta.key || metaIt->second.pid != pid) {
        continue;
      }
      iovds_.erase(target.meta.key);
      gpuShmsById.erase(parseResult->id);
      gpuIovMetaByIovd.erase(metaIt);
      removed = true;
    }

    if (removed) {
      iovs->dealloc(target.iovd);
    }
  }
#endif

  return ioRingIndexes;
}

std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
IovTable::listIovs(const meta::UserInfo &ui) {
  meta::DirEntry de{meta::InodeId::iovDir(), ""};

  auto n = iovs->slots.nextAvail.load();
  std::vector<meta::DirEntry> des;
  std::vector<std::optional<meta::Inode>> ins;
  des.reserve(n + 3);
  ins.reserve(n + 3);

  for (int prio = 0; prio <= 2; ++prio) {
    de.name = IoRingTable::semName(prio);
    des.emplace_back(de);

    auto inode = IoRingTable::lookupSem(prio);
    ins.emplace_back(std::move(inode));
  }

  meta::Acl acl{meta::Uid{ui.uid}, meta::Gid{ui.gid}, meta::Permission{0400}};

#ifdef HF3FS_GDR_ENABLED
  // Snapshot GPU metadata under a single lock before iterating
  robin_hood::unordered_map<int, GpuIovMeta> gpuMetaSnapshot;
  {
    std::lock_guard lock(gpuShmLock);
    gpuMetaSnapshot = gpuIovMetaByIovd;
  }
#endif

  for (int i = 0; i < n; ++i) {
    auto iov = iovs->table[i].load();
    if (iov) {
      if (iov->user != ui.uid) {
        continue;
      }
      de.name = iov->key;
      des.emplace_back(de);
      ins.emplace_back(
          meta::Inode{meta::InodeId{meta::InodeId::iov(i)}, meta::InodeData{meta::Symlink{linkPref / iov->path}, acl}});
      continue;
    }

#ifdef HF3FS_GDR_ENABLED
    // Check for GPU iov from snapshot
    auto git = gpuMetaSnapshot.find(i);
    if (git != gpuMetaSnapshot.end() && git->second.user == ui.uid) {
      de.name = git->second.key;
      des.emplace_back(de);
      ins.emplace_back(
          meta::Inode{meta::InodeId{meta::InodeId::iov(i)},
                      meta::InodeData{meta::Symlink{git->second.target},
                                      meta::Acl{git->second.user, git->second.gid, meta::Permission{0400}}}});
    }
#endif
  }

  return std::make_pair(std::make_shared<std::vector<meta::DirEntry>>(std::move(des)),
                        std::make_shared<std::vector<std::optional<meta::Inode>>>(std::move(ins)));
}
}  // namespace hf3fs::fuse
