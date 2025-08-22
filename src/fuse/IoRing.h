#pragma once

#include <cstdint>
#include <semaphore.h>

#include "IovTable.h"
#include "UserConfig.h"
#include "client/storage/StorageClient.h"
#include "common/utils/AtomicSharedPtrTable.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"

namespace hf3fs::fuse {
struct RcInode;
struct IoArgs {
  uint8_t bufId[16];
  size_t bufOff;

  uint64_t fileIid;
  size_t fileOff;

  uint64_t ioLen;

  const void *userdata;
};

struct IoSqe {
  int32_t index;
  const void *userdata;
};

struct IoCqe {
  int32_t index;
  int32_t reserved;
  int64_t result;
  const void *userdata;
};

class IoRing;

struct IoRingJob {
  std::shared_ptr<IoRing> ior;
  int sqeProcTail;
  int toProc;
};

// we allow multiple io workers to process the same ioring, but different ranges
// so 1 ioring can be used to submit ios processed in parallel
// however, we don't allow multiple threads to prepare ios in the same ioring
// or batches may be mixed and things may get ugly
class IoRing : public std::enable_shared_from_this<IoRing> {
 public:
  static int ringMarkerSize() {
    auto n = std::atomic_ref<int32_t>::required_alignment;
    return (4 + n - 1) / n * n;
  }
  // allocate 1 more slot for queue emptiness/fullness checking
  static int ioRingEntries(size_t bufSize) {
    auto n = ringMarkerSize();
    // n * 4 for sqe/cqe head/tail markers
    return (int)std::min((size_t)std::numeric_limits<int>::max(),
                         (bufSize - 4096 - n * 4 - sizeof(sem_t)) / (sizeof(IoArgs) + sizeof(IoCqe) + sizeof(IoSqe))) -
           1;
  }
  static size_t bytesRequired(int entries) {
    auto n = ringMarkerSize();
    // n * 4 for sqe/cqe head/tail markers
    return n * 4 + sizeof(sem_t) + (sizeof(IoArgs) + sizeof(IoCqe) + sizeof(IoSqe)) * (entries + 1) + 4096;
  }

 public:
  using std::enable_shared_from_this<IoRing>::shared_from_this;

  // the shm arg is used to keep it from being destroyed when the iov link is removed
  IoRing(std::shared_ptr<lib::ShmBuf> shm,
         std::string_view nm,
         const meta::UserInfo &ui,
         bool read,
         uint8_t *buf,
         size_t size,
         int iod,
         int prio,
         Duration to,
         uint64_t flags,
         bool owner = true)
      : name(nm),
        entries(ioRingEntries(size) + 1),
        ioDepth(iod),
        priority(prio),
        timeout(to),
        sqeHead_((int32_t *)buf),
        sqeTail_((int32_t *)(buf + ringMarkerSize())),
        cqeHead_((int32_t *)(buf + ringMarkerSize() * 2)),
        cqeTail_((int32_t *)(buf + ringMarkerSize() * 3)),
        sqeHead(*sqeHead_),
        sqeTail(*sqeTail_),
        cqeHead(*cqeHead_),
        cqeTail(*cqeTail_),
        ringSection((IoArgs *)(buf + ringMarkerSize() * 4)),
        cqeSection((IoCqe *)(ringSection + entries)),
        sqeSection((IoSqe *)(cqeSection + entries)),
        slots(entries - 1),
        shm_(std::move(shm)),
        userInfo_(ui),
        forRead_(read),
        flags_(flags) {
    XLOGF_IF(FATAL,
             (uintptr_t)(sqeSection + entries + sizeof(sem_t)) > (uintptr_t)(buf + size),
             "sem has a bad address {}, after whole shm starts at {} with {} bytes",
             (void *)(sqeSection + entries + sizeof(sem_t)),
             (void *)buf,
             size);
    auto sem = (sem_t *)(sqeSection + entries);
    if (owner) {
      sem_init(sem, 1, 0);
    }
    cqeSem.reset(sem);
  }
  std::vector<IoRingJob> jobsToProc(int maxJobs);
  int cqeCount() const { return (cqeHead.load() + entries - cqeTail.load()) % entries; }
  CoTask<void> process(
      int spt,
      int toProc,
      storage::client::StorageClient &storageClient,
      const storage::client::IoOptions &storageIo,
      UserConfig &userConfig,
      std::function<void(std::vector<std::shared_ptr<RcInode>> &, const IoArgs *, const IoSqe *, int)> &&lookupFiles,
      std::function<void(std::vector<Result<lib::ShmBufForIO>> &, const IoArgs *, const IoSqe *, int)> &&lookupBufs);

 public:
  bool addSqe(int idx, const void *userdata) {
    auto h = sqeHead.load();
    if ((h + 1) % entries == sqeTail.load()) {
      return false;
    }

    auto &sqe = sqeSection[h];
    sqe.index = idx;
    sqe.userdata = userdata;

    sqeHead.store((h + 1) % entries);

    return true;
  }
  bool sqeTailAfter(int a, int b) {
    auto h = sqeHead.load();
    if (a == h) {  // caught up with head, must be the last
      return true;
    }
    auto ah = a > h, bh = b > h;
    if (ah == bh) {  // both after or before head, bigger is after
      return a > b;
    } else {  // the one before head is after
      return bh;
    }
  }

 public:
  std::string name;
  std::string mountName;
  int entries;
  int ioDepth;
  int priority;
  Duration timeout;

 private:
  int32_t *sqeHead_;
  int32_t *sqeTail_;
  int32_t *cqeHead_;
  int32_t *cqeTail_;
  std::optional<SteadyTime> lastCheck_;

 public:
  std::atomic_ref<int32_t> sqeHead;
  std::atomic_ref<int32_t> sqeTail;
  std::atomic_ref<int32_t> cqeHead;
  std::atomic_ref<int32_t> cqeTail;
  IoArgs *ringSection;
  IoCqe *cqeSection;
  IoSqe *sqeSection;
  std::unique_ptr<sem_t, std::function<void(sem_t *)>> cqeSem{nullptr, [](sem_t *p) { sem_destroy(p); }};

 public:
  AvailSlots slots;

 private:
  int sqeCount() const { return (sqeHead.load() + entries - sqeProcTail_) % entries; }
  [[nodiscard]] bool addCqe(int idx, ssize_t res, const void *userdata) {
    auto h = cqeHead.load();
    if ((h + 1) % entries == cqeTail.load()) {
      return false;
    }

    auto &cqe = cqeSection[h];
    cqe.index = idx;
    cqe.result = res;
    cqe.userdata = userdata;

    cqeHead.store((h + 1) % entries);
    return true;
  }

 private:  // for fuse
  std::shared_ptr<lib::ShmBuf> shm_;
  meta::UserInfo userInfo_;
  bool forRead_;
  uint64_t flags_;
  std::mutex cqeMtx_;  // when reporting cqes
  int sqeProcTail_{0};
  int processing_{0};
  std::deque<int> sqeProcTails_;  // tails claimed and processing
  std::set<int> sqeDoneTails_;    // tails done processing
};

struct IoRingTable {
  void init(int cap) {
    for (int prio = 0; prio <= 2; ++prio) {
      auto sp = "/" + semOpenPath(prio);
      sems.emplace_back(sem_open(sp.c_str(), O_CREAT, 0666, 0), [sp](sem_t *p) {
        sem_close(p);
        sem_unlink(sp.c_str());
      });
      chmod(semPath(prio).c_str(), 0666);
    }
    ioRings = std::make_unique<AtomicSharedPtrTable<IoRing>>(cap);
  }
  Result<int> addIoRing(const Path &mountName,
                        std::shared_ptr<lib::ShmBuf> shm,
                        std::string_view name,
                        const meta::UserInfo &ui,
                        bool forRead,
                        uint8_t *buf,
                        size_t size,
                        int ioDepth,
                        const hf3fs::lib::IorAttrs &attrs) {
    auto idxRes = ioRings->alloc();
    if (!idxRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many io rings");
    }

    auto idx = *idxRes;

    auto ior = std::make_shared<
        IoRing>(std::move(shm), name, ui, forRead, buf, size, ioDepth, attrs.priority, attrs.timeout, attrs.flags);
    ior->mountName = mountName.native();
    ioRings->table[idx].store(ior);

    return idx;
  }
  void rmIoRing(int idx) { ioRings->remove(idx); }
  std::vector<std::unique_ptr<sem_t, std::function<void(sem_t *)>>> sems;
  std::unique_ptr<AtomicSharedPtrTable<IoRing>> ioRings;

 private:
  static std::string semOpenPath(int prio) {
    static std::vector<Uuid> semIds{Uuid::random(), Uuid::random(), Uuid::random()};
    return fmt::format("hf3fs-submit-ios.{}", semIds[prio].toHexString());
  }

 public:
  static std::string semName(int prio) {
    return fmt::format("submit-ios{}", prio == 1 ? "" : prio == 0 ? ".ph" : ".pl");
  }
  static Path semPath(int prio) { return Path("/dev/shm") / ("sem." + semOpenPath(prio)); }
  static meta::Inode lookupSem(int prio) {
    static const std::vector<meta::Inode> inodes{
        {meta::InodeId{meta::InodeId::iovDir().u64() - 1},
         meta::InodeData{meta::Symlink{semPath(0)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}},
        {meta::InodeId{meta::InodeId::iovDir().u64() - 2},
         meta::InodeData{meta::Symlink{semPath(1)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}},
        {meta::InodeId{meta::InodeId::iovDir().u64() - 3},
         meta::InodeData{meta::Symlink{semPath(2)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}}};

    return inodes[prio];
  }
};
}  // namespace hf3fs::fuse
