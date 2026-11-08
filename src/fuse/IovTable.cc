#include "IovTable.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <folly/experimental/coro/BlockingWait.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "IoRing.h"
#include "fbs/meta/Common.h"
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
  entries_ = std::make_unique<AtomicSharedPtrTable<IovEntry>>(cap);
}

Result<Void> IovTable::publishEntry(int iovd, std::shared_ptr<IovEntry> entry) {
  std::unique_lock lock(mutex_);
  if (!entries_ || iovd < 0 || iovd >= (int)entries_->table.size() || entries_->table[iovd].load()) {
    return makeError(StatusCode::kInvalidArg, "invalid iov descriptor");
  }
  if (iovdsByKey_.find(entry->key) != iovdsByKey_.end() || iovdsById_.find(entry->id) != iovdsById_.end()) {
    return makeError(MetaCode::kExists, "iov key or UUID already exists");
  }

  iovdsByKey_.emplace(entry->key, iovd);
  iovdsById_.emplace(entry->id, iovd);
  entries_->table[iovd].store(std::move(entry));
  return Void{};
}

bool IovTable::removeEntryLocked(int iovd, const std::shared_ptr<IovEntry> &expected) {
  if (!entries_ || iovd < 0 || iovd >= (int)entries_->table.size()) {
    return false;
  }

  auto current = entries_->table[iovd].load();
  if (!current || current != expected) {
    return false;
  }

  auto keyIt = iovdsByKey_.find(current->key);
  if (keyIt != iovdsByKey_.end() && keyIt->second == iovd) {
    iovdsByKey_.erase(keyIt);
  }
  auto idIt = iovdsById_.find(current->id);
  if (idIt != iovdsById_.end() && idIt->second == iovd) {
    iovdsById_.erase(idIt);
  }
  entries_->remove(iovd);
  return true;
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

  {
    std::shared_lock lock(mutex_);
    if (iovdsByKey_.find(key) != iovdsByKey_.end() || iovdsById_.find(iovaRes->id) != iovdsById_.end()) {
      return makeError(MetaCode::kExists, "iov key or UUID already exists");
    }
  }

#ifndef HF3FS_ENABLE_GDR
  if (iovaRes->isGdr) {
    return makeError(StatusCode::kInvalidArg, "GDR not enabled in this build");
  }
#endif

#ifdef HF3FS_ENABLE_GDR
  // GDR path: shmPath is a gdr:// URI, not a filesystem path
  if (iovaRes->isGdr) {
    auto gdrTarget = lib::parseGdrUri(shmPath.native());
    if (!gdrTarget) {
      return makeError(StatusCode::kInvalidArg, "failed to parse GDR target URI");
    }
    if (iovaRes->gpuDeviceId != gdrTarget->deviceId) {
      return MAKE_ERROR_F(StatusCode::kInvalidArg,
                          "gdr key device {} does not match URI device {}",
                          iovaRes->gpuDeviceId,
                          gdrTarget->deviceId);
    }

    lib::CudaIpcHandle ipcHandle;
    std::copy(gdrTarget->ipcHandle.begin(), gdrTarget->ipcHandle.end(), ipcHandle.begin());
    auto gpuShmResult = lib::GpuShmBuf::create(ipcHandle,
                                               gdrTarget->allocationSize,
                                               gdrTarget->offset,
                                               gdrTarget->size,
                                               gdrTarget->deviceId);
    RETURN_ON_ERROR(gpuShmResult);
    auto gpuShm = std::move(*gpuShmResult);

    // Allocate iov descriptor slot
    auto iovdRes = entries_->alloc();
    if (!iovdRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many iovs allocated");
    }
    auto iovd = *iovdRes;
    bool dealloc = true;
    SCOPE_EXIT {
      if (dealloc) {
        entries_->dealloc(iovd);
      }
    };

    auto entry = std::make_shared<IovEntry>(
        IovEntry{std::string(key), iovaRes->id, shmPath, ui.uid, ui.gid, pid, IovBuffer{gpuShm}});
    RETURN_ON_ERROR(publishEntry(iovd, entry));

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
    auto iovdRes = entries_->alloc();
    if (!iovdRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many iovs allocated");
    }
    auto iovd = *iovdRes;
    bool dealloc = true;
    SCOPE_EXIT {
      if (dealloc) {
        entries_->dealloc(iovd);
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

    start = SteadyClock::now();
    auto recordMetrics = [blockSize = shm->blockSize, start, uids]() mutable {
      ibRegBytesDist.addSample(blockSize, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
      ibRegLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
    };

    if (!iovaRes->isIoRing) {  // io ring bufs don't need to be registered for ib io
      folly::coro::blockingWait(shm->registerForIO(exec, sc, recordMetrics));
    }

    auto entry = std::make_shared<IovEntry>(
        IovEntry{std::string(key), iovaRes->id, linkPref / shm->path, ui.uid, ui.gid, pid, IovBuffer{shm}});
    RETURN_ON_ERROR(publishEntry(iovd, entry));

    auto statRes = statIov(iovd, ui);
    RETURN_ON_ERROR(statRes);

    dealloc = false;
    return std::make_pair(*statRes, iovaRes->isIoRing ? shm : std::shared_ptr<lib::ShmBuf>());
  }
}

Result<std::shared_ptr<lib::ShmBuf>> IovTable::rmIov(const char *key,
                                                     const meta::UserInfo &ui,
                                                     const std::shared_ptr<lib::ShmBuf> &expectedBuffer) {
  std::shared_ptr<IovEntry> entry;
  int iovd = -1;
  {
    std::unique_lock lock(mutex_);
    auto it = iovdsByKey_.find(key);
    if (it == iovdsByKey_.end() || !entries_) {
      return makeError(MetaCode::kNotFound, std::string("iov key not found ") + key);
    }

    iovd = it->second;
    entry = entries_->table[iovd].load();
    if (!entry || entry->key != key) {
      return makeError(MetaCode::kNotFound, std::string("iov key not found ") + key);
    }
    if (entry->user != ui.uid) {
      XLOGF(ERR, "removing user {} iov belongs to {}", ui.uid, entry->user);
      return makeError(MetaCode::kNoPermission, "iov not for user");
    }
    if (expectedBuffer) {
      auto host = std::get_if<std::shared_ptr<lib::ShmBuf>>(&entry->buffer);
      if (!host || *host != expectedBuffer) {
        return makeError(MetaCode::kNotFound, "iov changed before rollback");
      }
    }
    XLOGF_IF(FATAL, !removeEntryLocked(iovd, entry), "iov entry changed while holding the table lock");
  }

#ifdef HF3FS_ENABLE_GDR
  if (entry->isGpu()) {
    // RDMABuf deregisters every MR before releasing its CUDA IPC mapping owner.
    entry.reset();
    return std::shared_ptr<lib::ShmBuf>();
  }
#endif

  return std::get<std::shared_ptr<lib::ShmBuf>>(entry->buffer);
}

Result<meta::Inode> IovTable::statIov(int iovd, const meta::UserInfo &ui) {
  std::shared_lock lock(mutex_);
  if (!entries_ || iovd < 0 || iovd >= (int)entries_->table.size()) {
    return makeError(MetaCode::kNotFound, "invalid iov desc");
  }

  auto entry = entries_->table[iovd].load();
  if (!entry) {
    return makeError(MetaCode::kNotFound,
                     fmt::format("iov desc {} not found, next avail {}", iovd, entries_->slots.nextAvail.load()));
  }

  if (entry->user != ui.uid) {
    XLOGF(ERR, "statting user {} iov belongs to {}", ui.uid, entry->user);
    return makeError(MetaCode::kNoPermission, "iov not for user");
  }

  return meta::Inode{
      meta::InodeId::iov(iovd),
      meta::InodeData{meta::Symlink{entry->target}, meta::Acl{entry->user, entry->gid, meta::Permission(0400)}}};
}

Result<meta::Inode> IovTable::lookupIov(const char *key, const meta::UserInfo &ui) {
  std::shared_lock lock(mutex_);
  auto it = iovdsByKey_.find(key);
  if (it == iovdsByKey_.end() || !entries_) {
    return makeError(MetaCode::kNotFound, std::string("iov key not found ") + key);
  }

  auto entry = entries_->table[it->second].load();
  if (!entry || entry->key != key) {
    return makeError(MetaCode::kNotFound, std::string("iov key not found ") + key);
  }
  if (entry->user != ui.uid) {
    XLOGF(ERR, "looking up user {} iov belongs to {}", ui.uid, entry->user);
    return makeError(MetaCode::kNoPermission, "iov not for user");
  }

  return meta::Inode{
      meta::InodeId::iov(it->second),
      meta::InodeData{meta::Symlink{entry->target}, meta::Acl{entry->user, entry->gid, meta::Permission(0400)}}};
}

namespace {

Result<IoBufForIO> makeIoBufForIO(const std::shared_ptr<IovEntry> &entry, const IovLookupRequest &request) {
  return std::visit(
      [&](const auto &buffer) -> Result<IoBufForIO> {
        using Buffer = typename std::decay_t<decltype(buffer)>::element_type;
        if (!buffer || request.offset > buffer->size || request.length > buffer->size - request.offset) {
          return makeError(StatusCode::kInvalidArg, "invalid buf off and/or io len");
        }

        if constexpr (std::is_same_v<Buffer, lib::ShmBuf>) {
          return IoBufForIO{lib::ShmBufForIO(buffer, request.offset)};
        }
#ifdef HF3FS_ENABLE_GDR
        else {
          return IoBufForIO{lib::GpuShmBufForIO(buffer, request.offset)};
        }
#endif
      },
      entry->buffer);
}

}  // namespace

void IovTable::lookupBufs(std::vector<Result<IoBufForIO>> &output,
                          std::span<const IovLookupRequest> requests,
                          meta::Uid requester) const {
  output.reserve(output.size() + requests.size());
  std::shared_lock lock(mutex_);

  bool hasLast = false;
  bool lastDenied = false;
  Uuid lastId = Uuid::zero();
  std::shared_ptr<IovEntry> lastEntry;
  for (const auto &request : requests) {
    if (!hasLast || request.id != lastId) {
      hasLast = true;
      lastDenied = false;
      lastId = request.id;
      lastEntry.reset();

      if (entries_) {
        auto it = iovdsById_.find(request.id);
        if (it != iovdsById_.end()) {
          auto entry = entries_->table[it->second].load();
          if (entry && entry->id == request.id && entry->user == requester) {
            lastEntry = std::move(entry);
          } else if (entry && entry->id == request.id) {
            lastDenied = true;
          }
        }
      }
    }

    if (!lastEntry) {
      output.emplace_back(lastDenied ? makeError(MetaCode::kNoPermission, "iov not for user")
                                     : makeError(StatusCode::kInvalidArg, "buf id not found"));
      continue;
    }
    output.emplace_back(makeIoBufForIO(lastEntry, request));
  }
}

Result<IoBufForIO> IovTable::lookupBuf(const IovLookupRequest &request, meta::Uid requester) const {
  std::vector<Result<IoBufForIO>> result;
  lookupBufs(result, std::span<const IovLookupRequest>(&request, 1), requester);
  return std::move(result.front());
}

std::shared_ptr<const IovEntry> IovTable::entryAt(int iovd) const {
  std::shared_lock lock(mutex_);
  if (!entries_ || iovd < 0 || iovd >= (int)entries_->table.size()) {
    return nullptr;
  }
  return entries_->table[iovd].load();
}

std::vector<int> IovTable::removeIovsByPid(pid_t pid) {
  struct IovToRemove {
    int iovd;
    std::shared_ptr<IovEntry> entry;
    std::optional<int> ioRingIndex;
  };

  std::vector<IovToRemove> targets;
  {
    std::shared_lock lock(mutex_);
    if (!entries_) {
      return {};
    }

    auto n = entries_->slots.nextAvail.load();
    targets.reserve(n);
    for (int i = 0; i < n; ++i) {
      auto entry = entries_->table[i].load();
      if (!entry || entry->pid != pid) {
        continue;
      }

      std::optional<int> ioRingIndex;
      if (auto host = std::get_if<std::shared_ptr<lib::ShmBuf>>(&entry->buffer); host && *host && (*host)->isIoRing) {
        ioRingIndex = (*host)->iorIndex;
      }
      targets.push_back(IovToRemove{i, std::move(entry), ioRingIndex});
    }
  }

  std::vector<int> ioRingIndexes;
  ioRingIndexes.reserve(targets.size());
  for (const auto &target : targets) {
    bool removed = false;
    {
      std::unique_lock lock(mutex_);
      if (target.entry->pid == pid) {
        removed = removeEntryLocked(target.iovd, target.entry);
      }
    }
    if (!removed) {
      XLOGF(DBG, "iov {} changed before dead pid {} cleanup", target.entry->key, pid);
      continue;
    }
    XLOGF(INFO, "unlinked iov {} symlink from dead pid {}", target.entry->key, pid);
    if (target.ioRingIndex) {
      ioRingIndexes.push_back(*target.ioRingIndex);
    }
  }
  return ioRingIndexes;
}

std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
IovTable::listIovs(const meta::UserInfo &ui) {
  meta::DirEntry de{meta::InodeId::iovDir(), ""};

  std::shared_lock lock(mutex_);
  auto n = entries_ ? entries_->slots.nextAvail.load() : 0;
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

  for (int i = 0; i < n; ++i) {
    auto entry = entries_->table[i].load();
    if (!entry || entry->user != ui.uid) {
      continue;
    }

    de.name = entry->key;
    des.emplace_back(de);
    ins.emplace_back(meta::Inode{
        meta::InodeId{meta::InodeId::iov(i)},
        meta::InodeData{meta::Symlink{entry->target}, meta::Acl{entry->user, entry->gid, meta::Permission{0400}}}});
  }

  return std::make_pair(std::make_shared<std::vector<meta::DirEntry>>(std::move(des)),
                        std::make_shared<std::vector<std::optional<meta::Inode>>>(std::move(ins)));
}

#ifdef HF3FS_ENABLE_GDR
void IovTable::clearGpuIovs() {
  std::vector<std::shared_ptr<lib::GpuShmBuf>> gpuBuffers;
  {
    std::unique_lock lock(mutex_);
    if (!entries_) {
      return;
    }

    auto n = entries_->slots.nextAvail.load();
    gpuBuffers.reserve(n);
    for (int i = 0; i < n; ++i) {
      auto entry = entries_->table[i].load();
      if (!entry) {
        continue;
      }
      auto gpu = std::get_if<std::shared_ptr<lib::GpuShmBuf>>(&entry->buffer);
      if (!gpu) {
        continue;
      }

      auto keyIt = iovdsByKey_.find(entry->key);
      if (keyIt != iovdsByKey_.end() && keyIt->second == i) {
        iovdsByKey_.erase(keyIt);
      }
      auto idIt = iovdsById_.find(entry->id);
      if (idIt != iovdsById_.end() && idIt->second == i) {
        iovdsById_.erase(idIt);
      }
      if (*gpu) {
        gpuBuffers.push_back(*gpu);
      }
      entries_->remove(i);
    }
  }

  gpuBuffers.clear();
}
#endif
}  // namespace hf3fs::fuse
