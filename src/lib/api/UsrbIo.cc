#include <cstdint>
#include <fcntl.h>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <folly/ScopeGuard.h>
#include <iostream>
#include <numa.h>
#include <sys/stat.h>

#include "common/logging/LogInit.h"
#include "common/utils/Duration.h"
#include "common/utils/Path.h"
#include "fuse/IoRing.h"
#include "lib/api/fuse.h"
#include "lib/api/hf3fs.h"
#include "lib/api/hf3fs_usrbio.h"
#include "lib/common/Shm.h"

struct Hf3fsInitLib {
  Hf3fsInitLib() {
    auto v = getenv("HF3FS_USRBIO_LIB_LOG");
    hf3fs::logging::initOrDie(v && *v ? v : "WARN");
  }
};
static Hf3fsInitLib initLib;

struct Hf3fsLibAliveness {
  std::mutex mtx;
  std::map<std::string, int> mountFds;
};

static Hf3fsLibAliveness alive;

bool hf3fs_is_hf3fs(int fd) {
  uint32_t magic = 0;

  auto res = ioctl(fd, hf3fs::lib::fuse::HF3FS_IOC_GET_MAGIC_NUM, &magic);
  if (res) {
    return false;
  }

  return magic == HF3FS_SUPER_MAGIC;
}

int hf3fs_extract_mount_point(char *hf3fs_mount_point, int size, const char *path) {
  auto s = getenv("HF3FS_USRBIO_DONT_CHECKFS_FOR_MP");
  auto checkFs = !(s && !strcmp(s, "yes"));

  std::set<boost::filesystem::path> mps;

  auto fp = fopen("/proc/self/mountinfo", "r");
  XLOGF_IF(FATAL, !fp, "cannot read system mount info");
  SCOPE_EXIT { fclose(fp); };

  char line[4096];
  std::vector<std::string> parts;
  while (true) {
    auto cp = fgets(line, sizeof(line), fp);
    if (!cp) {
      break;
    }

    // there are two kinds of mounts:
    // 1. directly mount the whole hf3fs
    // 2. mount some subdirectories separately
    // for case 1, we find the root of the fs directly in the mountinfo
    // for case 2, we find the parent of the 3fs-virt dir

    parts.clear();
    folly::split(' ', line, parts, true);

    if (parts.size() < 10) {
      continue;
    }

    if (checkFs && parts[parts.size() - 3] != "fuse.hf3fs") {
      continue;
    }

    auto mp = boost::filesystem::path(parts[4]).lexically_normal();
    if (mp.filename() == "3fs-virt") {
      mps.insert(mp.parent_path());
    } else {
      try {
        if (boost::filesystem::exists(mp / "3fs-virt")) {
          mps.insert(mp);
        }
      } catch (const boost::filesystem::filesystem_error &) {
        // ignore
      }
    }
  }

  auto rp = boost::filesystem::canonical(boost::filesystem::path(path)).lexically_normal();
  for (auto it = mps.crbegin(); it != mps.crend(); ++it) {
    auto &mp = *it;
    auto [mpm, rpm] = std::mismatch(mp.begin(), mp.end(), rp.begin(), rp.end());
    if (mpm == mp.end()) {
      if ((int)mp.size() < size) {
        strcpy(hf3fs_mount_point, mp.c_str());
      }
      return mp.size() + 1;
    }
  }

  return -1;
}

int hf3fs_iovcreate_general(struct hf3fs_iov *iov,
                            const char *hf3fs_mount_point,
                            size_t size,
                            size_t block_size,
                            int numa,
                            bool is_io_ring,
                            bool for_read,
                            int io_depth,
                            int priority = 0,
                            int timeout = 0,
                            uint64_t flags = 0) {
  if (!iov) {
    return -EINVAL;
  }

  if (strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    XLOGF(ERR, "mount point too long '{}'", hf3fs_mount_point);
    return -EINVAL;
  }

  auto p = fmt::format("/hf3fs-iov-{}", hf3fs::Uuid::random());

  hf3fs::lib::ShmBuf *shm;
  try {
    shm = new hf3fs::lib::ShmBuf(p, size, block_size, numa, hf3fs::meta::Uid(getuid()), getpid(), getppid());
  } catch (const std::runtime_error &e) {
    XLOGF(ERR, "failed to create/map shm for iov {}", e.what());
    return -EIO;
  }

  bool succ = false;
  SCOPE_EXIT {
    if (!succ) {
      delete shm;
    }
  };

  auto target = hf3fs::Path("/dev/shm") / p;
  auto link = fmt::format("{}/3fs-virt/iovs/{}{}{}{}{}{}",
                          hf3fs_mount_point,
                          shm->id.toHexString(),
                          block_size ? fmt::format(".b{}", block_size) : std::string(),
                          is_io_ring ? fmt::format(".{}{}", for_read ? 'r' : 'w', io_depth) : std::string(),
                          is_io_ring && priority != 0 ? fmt::format(".p{}", priority < 0 ? 'h' : 'l') : std::string(),
                          is_io_ring ? fmt::format(".t{}", timeout) : std::string(),
                          is_io_ring && flags != 0 ? fmt::format(".f{:b}", flags) : std::string());
  auto lres = symlink(target.c_str(), link.c_str());
  if (lres < 0) {
    XLOGF(ERR, "failed to register iov '{}' to hf3fs '{}'", target, link);
    return -errno;
  }
  if (is_io_ring) {
    shm->maybeUnlinkShm();
  }

  iov->base = shm->bufStart;
  iov->iovh = shm;
  memcpy(iov->id, shm->id.data, sizeof(iov->id));

  strcpy(iov->mount_point, hf3fs_mount_point);
  iov->size = size;
  iov->block_size = block_size;
  iov->numa = numa;

  succ = true;

  std::lock_guard lock(alive.mtx);
  if (alive.mountFds.find(hf3fs_mount_point) == alive.mountFds.end()) {
    auto fd = open(fmt::format("{}/3fs-virt/iovs", hf3fs_mount_point).c_str(), O_DIRECTORY);
    alive.mountFds[hf3fs_mount_point] = fd;
    XLOGF(INFO, "fd {} for mount {}", fd, hf3fs_mount_point);
  }

  return 0;
}

void hf3fs_iovdestroy_general(struct hf3fs_iov *iov,
                              bool is_io_ring,
                              bool for_read,
                              int io_depth,
                              int priority = 0,
                              int timeout = 0,
                              uint64_t flags = 0) {
  if (!iov) {
    return;
  }

  if (!iov->iovh) {  // not allocated by hf3fs_iovalloc(), cannot be freed by hf3fs_iovfree()
    if (!is_io_ring) {
      XLOGF(ERR, "cannot iovfree() an iov created by iovwrap()");
    }
    return;
  }

  hf3fs::Uuid id;
  memcpy(id.data, iov->id, sizeof(id.data));

  auto link = fmt::format("{}/3fs-virt/iovs/{}{}{}{}{}{}",
                          iov->mount_point,
                          id.toHexString(),
                          iov->block_size ? fmt::format(".b{}", iov->block_size) : std::string(),
                          is_io_ring ? fmt::format(".{}{}", for_read ? 'r' : 'w', io_depth) : std::string(),
                          is_io_ring && priority != 0 ? fmt::format(".p{}", priority < 0 ? 'h' : 'l') : std::string(),
                          is_io_ring ? fmt::format(".t{}", timeout) : std::string(),
                          is_io_ring && flags != 0 ? fmt::format(".f{}", flags) : std::string());
  unlink(link.c_str());

  auto *shm = static_cast<hf3fs::lib::ShmBuf *>(iov->iovh);
  if (!is_io_ring) {
    shm->maybeUnlinkShm();
  }
  delete (hf3fs::lib::ShmBuf *)iov->iovh;
  iov->iovh = nullptr;
}

int hf3fs_iovcreate(struct hf3fs_iov *iov, const char *hf3fs_mount_point, size_t size, size_t block_size, int numa) {
  return hf3fs_iovcreate_general(iov, hf3fs_mount_point, size, block_size, numa, false, true, 0);
}

int hf3fs_iovopen(struct hf3fs_iov *iov,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa) {
  hf3fs::Uuid uuid;
  memcpy(uuid.data, id, sizeof(uuid.data));

  auto link = fmt::format("{}/3fs-virt/iovs/{}{}",
                          hf3fs_mount_point,
                          uuid.toHexString(),
                          block_size ? fmt::format(".b{}", block_size) : std::string());
  auto shm_path_c = realpath(link.c_str(), nullptr);
  if (!shm_path_c) {
    XLOGF(ERR, "hf3fs_iovopen realpath failed with errno {}", errno);
    return -errno;
  }
  auto shm_path = std::string(shm_path_c);
  free(shm_path_c);

  std::string prefix("/dev/shm");
  if (shm_path.substr(0, prefix.size()) != prefix) {
    XLOGF(ERR, "hf3fs_iovopen shm_path is not in /dev/shm");
    return -EINVAL;
  }
  shm_path = shm_path.substr(prefix.size());

  hf3fs::lib::ShmBuf *shm;
  try {
    shm = new hf3fs::lib::ShmBuf(shm_path, 0, size, block_size, uuid);
  } catch (const std::runtime_error &e) {
    XLOGF(ERR, "hf3fs_iovopen failed to map shm for iov {}", e.what());
    return -EIO;
  }

  bool succ = false;
  SCOPE_EXIT {
    if (!succ) {
      delete shm;
    }
  };

  iov->base = shm->bufStart;
  iov->iovh = shm;
  memcpy(iov->id, shm->id.data, sizeof(iov->id));

  strcpy(iov->mount_point, hf3fs_mount_point);
  iov->size = size;
  iov->block_size = block_size;
  iov->numa = numa;

  succ = true;

  return 0;
}

void hf3fs_iovunlink(struct hf3fs_iov *iov) {
  auto *shm = static_cast<hf3fs::lib::ShmBuf *>(iov->iovh);
  shm->maybeUnlinkShm();
}

void hf3fs_iovdestroy(struct hf3fs_iov *iov) { hf3fs_iovdestroy_general(iov, false, true, 0); }

size_t hf3fs_ior_size(int entries) { return hf3fs::fuse::IoRing::bytesRequired(entries); }

int hf3fs_iovwrap(struct hf3fs_iov *iov,
                  void *buf,
                  const uint8_t id[16],
                  const char *hf3fs_mount_point,
                  size_t size,
                  size_t block_size,
                  int numa) {
  if (!iov) {
    return -EINVAL;
  }

  if (strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    XLOGF(ERR, "mount point too long '{}'", hf3fs_mount_point);
    return -EINVAL;
  }

  iov->iovh = nullptr;

  iov->base = (uint8_t *)buf;
  memcpy(iov->id, id, sizeof(iov->id));

  strcpy(iov->mount_point, hf3fs_mount_point);
  iov->size = size;
  iov->block_size = block_size;
  iov->numa = numa;

  if (numa >= 0) {
    numa_tonode_memory(buf, size, numa);
  }

  return 0;
}

struct Hf3fsIorHandle {
  std::unique_ptr<hf3fs::fuse::IoRing> ior;
  sem_t *submitSem;
};

static int cqeSem(sem_t *&sem, const char *hf3fs_mount_point, int prio) {
  auto link = fmt::format("{}/3fs-virt/iovs/submit-ios{}",
                          std::string(hf3fs_mount_point),
                          prio == 0  ? ""
                          : prio < 0 ? ".ph"
                                     : ".pl");
  std::vector<char> target(256);

  while (true) {
    auto lres = readlink(link.c_str(), target.data(), target.size());
    if (lres < 0) {
      return -errno;
    } else if (lres >= (ssize_t)target.size()) {
      XLOGF(ERR, "hf3fs reports strange link target for submit sem");
      return -EIO;
    } else {
      break;
    }
  }

  auto semPath = hf3fs::Path(target).lexically_normal();
  static const auto devShm = hf3fs::Path("/dev/shm");

  auto [sm, pm] = std::mismatch(devShm.begin(), devShm.end(), semPath.begin(), semPath.end());
  auto it = pm;
  if (sm != devShm.end() || it == semPath.end() || ++it != semPath.end() || pm->native().size() <= 4 ||
      pm->native().substr(0, 4) != "sem.") {
    XLOGF(ERR, "invalid submit sem name from hf3fs '{}'", semPath);
    return -EIO;
  }

  auto semName = std::string("/") + (pm->c_str() + 4);
  sem = sem_open(semName.c_str(), 0);
  if (!sem) {
    return -errno;
  }

  return 0;
}

int hf3fs_iorwrap(struct hf3fs_ior *ior,
                  void *buf,
                  const char *hf3fs_mount_point,
                  size_t size,
                  bool for_read,
                  int io_depth,
                  int priority,
                  int timeout,
                  uint64_t flags) {
  if (!ior || !buf || !hf3fs_mount_point || !*hf3fs_mount_point || !hf3fs::fuse::IoRing::ioRingEntries(size) ||
      timeout < 0) {
    return -EINVAL;
  }

  auto iorh = std::make_unique<Hf3fsIorHandle>(
      Hf3fsIorHandle{std::make_unique<hf3fs::fuse::IoRing>(
                         std::shared_ptr<hf3fs::lib::ShmBuf>{},
                         "",
                         hf3fs::meta::UserInfo{hf3fs::meta::Uid{geteuid()}, hf3fs::meta::Gid{getegid()}, ""},
                         for_read,
                         (uint8_t *)buf,
                         size,
                         io_depth,
                         priority,
                         hf3fs::Duration(std::chrono::nanoseconds((uint64_t)timeout * 1000000)),
                         flags,
                         false),
                     nullptr});
  if (strlen(hf3fs_mount_point) >= sizeof(ior->mount_point)) {
    XLOGF(ERR, "mount point too long '{}'", hf3fs_mount_point);
    return -EINVAL;
  }

  auto res = cqeSem(iorh->submitSem, hf3fs_mount_point, priority);
  if (res < 0) {
    return res;
  }

  ior->iorh = iorh.release();

  strcpy(ior->mount_point, hf3fs_mount_point);
  ior->for_read = for_read;
  ior->io_depth = io_depth;
  ior->priority = priority;
  ior->timeout = timeout;
  ior->flags = flags;
  ior->iov.size = size;
  return 0;
}

int hf3fs_iorcreate(struct hf3fs_ior *ior,
                    const char *hf3fs_mount_point,
                    int entries,
                    bool for_read,
                    int io_depth,
                    int numa) {
  return hf3fs_iorcreate2(ior, hf3fs_mount_point, entries, for_read, io_depth, 0, numa);
}

int hf3fs_iorcreate2(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int priority,
                     int numa) {
  return hf3fs_iorcreate3(ior, hf3fs_mount_point, entries, for_read, io_depth, priority, 0, numa);
}

int hf3fs_iorcreate3(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int priority,
                     int timeout,
                     int numa) {
  if (!ior || !hf3fs_mount_point || !*hf3fs_mount_point || timeout < 0) {
    return -EINVAL;
  }

  auto iov_size = hf3fs_ior_size(entries);
  auto res = hf3fs_iovcreate_general(&ior->iov,
                                     hf3fs_mount_point,
                                     iov_size,
                                     0,
                                     numa,
                                     true,
                                     for_read,
                                     io_depth,
                                     priority,
                                     timeout);
  if (res < 0) {
    XLOGF(ERR, "ioring create failed: hf3fs_iovcreate_general failed {}", res);
    return res;
  }

  res = hf3fs_iorwrap(ior, ior->iov.base, hf3fs_mount_point, ior->iov.size, for_read, io_depth, priority, timeout, 0);
  if (res < 0) {
    hf3fs_iovdestroy(&ior->iov);
    XLOGF(ERR, "ioring create failed: hf3fs_iorwrap failed {}", res);
    return res;
  }

  return 0;
}

int hf3fs_iorcreate4(struct hf3fs_ior *ior,
                     const char *hf3fs_mount_point,
                     int entries,
                     bool for_read,
                     int io_depth,
                     int timeout,
                     int numa,
                     uint64_t flags) {
  if (!ior || !hf3fs_mount_point || !*hf3fs_mount_point || timeout < 0) {
    return -EINVAL;
  }

  auto iov_size = hf3fs_ior_size(entries);
  auto res = hf3fs_iovcreate_general(&ior->iov,
                                     hf3fs_mount_point,
                                     iov_size,
                                     0,
                                     numa,
                                     true,
                                     for_read,
                                     io_depth,
                                     0,
                                     timeout,
                                     flags);
  if (res < 0) {
    XLOGF(ERR, "ioring create failed: hf3fs_iovcreate_general failed {}", res);
    return res;
  }

  res = hf3fs_iorwrap(ior, ior->iov.base, hf3fs_mount_point, ior->iov.size, for_read, io_depth, 0, timeout, flags);
  if (res < 0) {
    hf3fs_iovdestroy(&ior->iov);
    XLOGF(ERR, "ioring create failed: hf3fs_iorwrap failed {}", res);
    return res;
  }

  return 0;
}

void hf3fs_iordestroy(struct hf3fs_ior *ior) {
  if (!ior) {
    return;
  }

  hf3fs_iovdestroy_general(&ior->iov, true, ior->for_read, ior->io_depth, ior->priority, ior->timeout, ior->flags);

  if (ior->iorh) {
    delete (Hf3fsIorHandle *)ior->iorh;
    ior->iorh = nullptr;
  }
}

struct Hf3fsRegisteredFd {
  Hf3fsRegisteredFd(int f, int df, hf3fs::meta::InodeId i, int s)
      : fd(f),
        dupfd(df),
        iid(i),
        status(s) {}
  ~Hf3fsRegisteredFd() { close(dupfd); }

  int fd;
  int dupfd;
  hf3fs::meta::InodeId iid;
  int status;
};

static int noFiles() {
  struct rlimit lim;
  auto ret = getrlimit(RLIMIT_NOFILE, &lim);
  if (ret < 0) {
    XLOGF(FATAL, "cannot get limit of number of open files");
  }

  return lim.rlim_max;
}

using Hf3fsRegisteredFds = std::vector<folly::atomic_shared_ptr<Hf3fsRegisteredFd>>;
static Hf3fsRegisteredFds regfds(noFiles());

int hf3fs_reg_fd(int fd, uint64_t flags) {
  (void)flags;

  auto is3fs = hf3fs_is_hf3fs(fd);
  if (!is3fs || fd >= (int)regfds.size()) {
    return EBADF;
  } else if (regfds[fd].load()) {
    return EINVAL;
  }

  struct statx stx;
  auto sres = statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_INO, &stx);
  if (sres < 0) {
    return errno;
  }

  auto dupfd = dup(fd);
  if (dupfd < 0) {
    return errno;
  } else if (regfds[dupfd].load()) {
    close(dupfd);
    return EINVAL;
  }

  int status = fcntl(fd, F_GETFL);

  std::shared_ptr<Hf3fsRegisteredFd> empty;
  auto regfd = std::make_shared<Hf3fsRegisteredFd>(fd, dupfd, hf3fs::meta::InodeId{stx.stx_ino}, status);
  if (!regfds[fd].compare_exchange_strong(empty, regfd)) {
    return EINVAL;  // already registered by another thread
  }
  if (!regfds[dupfd].compare_exchange_strong(empty, regfd)) {
    empty.reset();
    regfds[fd].store(empty);
    return EINVAL;
  }

  return -dupfd;
}

void hf3fs_dereg_fd(int fd) {
  auto is3fs = hf3fs_is_hf3fs(fd);
  if (!is3fs || fd < 0 || fd >= (int)regfds.size()) {
    return;
  }

  std::shared_ptr<Hf3fsRegisteredFd> empty;
  auto regfd = regfds[fd].load();
  if (!regfd) {
    return;
  }
  auto regfd2 = regfd;
  regfds[regfd->dupfd].compare_exchange_strong(regfd2, empty);
  regfds[fd].compare_exchange_strong(regfd, empty);
}

int hf3fs_io_entries(const struct hf3fs_ior *ior) { return hf3fs::fuse::IoRing::ioRingEntries(ior->iov.size); }

int hf3fs_prep_io(const struct hf3fs_ior *ior,
                  const struct hf3fs_iov *iov,
                  bool read,
                  void *ptr,
                  int fd,
                  size_t off,
                  uint64_t len,
                  const void *userdata) {
  auto p = (uint8_t *)ptr;
  auto afd = abs(fd);
  if (!ior || !ior->iorh || read != ior->for_read || !iov || len <= 0 || !iov->base || p < iov->base ||
      p + len > iov->base + iov->size || afd >= (int)regfds.size()) {
    return -EINVAL;
  }

  auto regfd = regfds[afd].load();
  if (!regfd) {  // fd not registered
    return -EBADF;
  }

  int status = regfd->status;
  if ((read && (status & O_ACCMODE) == O_WRONLY) || (!read && (status & O_ACCMODE) == O_RDONLY)) {
    return -EACCES;
  }

  auto &iorh = *(Hf3fsIorHandle *)ior->iorh;
  auto &ring = *iorh.ior;

  auto idx = ring.slots.alloc();
  if (!idx) {  // ring is full
    return -EAGAIN;
  }

  auto &args = ring.ringSection[*idx];
  memcpy(args.bufId, iov->id, sizeof(iov->id));
  args.bufOff = p - iov->base;
  args.fileIid = regfd->iid.u64();
  args.fileOff = off;
  args.ioLen = len;
  args.userdata = userdata;

  {
    auto res = ring.addSqe(*idx, userdata);
    if (!res) {
      XLOGF(ERR, "no more sqes when args are added");
      ring.slots.dealloc(*idx);
      return -EAGAIN;
    }
  }

  hf3fs::Uuid id;
  memcpy(id.data, ior->iov.id, sizeof(id.data));

  return *idx;
}

int hf3fs_submit_ios(const struct hf3fs_ior *ior) {
  if (!ior || !ior->iorh) {
    return -EINVAL;
  }

  auto &iorh = *(Hf3fsIorHandle *)ior->iorh;
  sem_post(iorh.submitSem);

  return 0;
}

int hf3fs_wait_for_ios(const struct hf3fs_ior *ior,
                       struct hf3fs_cqe *cqes,
                       int cqec,
                       int min_results,
                       const struct timespec *abs_timeout) {
  if (cqec <= 0 || !ior || !ior->iorh) {
    return -EINVAL;
  }

  if (min_results > cqec) {
    min_results = cqec;
  }

  auto jitter = 1;
  auto js = getenv("HF3FS_USRBIO_WAIT_JITTER_MS");
  if (js && atoi(js)) {
    jitter = atoi(js);
  }

  auto &iorh = *(Hf3fsIorHandle *)ior->iorh;
  auto &ring = *iorh.ior;

  int filled = 0;
  do {
    auto done = ring.cqeCount();
    if (done) {
      done = std::min(done, cqec - filled);
      for (auto i = 0; i < done; ++i) {
        auto t = ring.cqeTail.load();
        if (t == ring.cqeHead.load()) {  // empty, drained by another consumer?
          break;
        }
        const auto &cqe = ring.cqeSection[t];

        // first record the info in curr cqe tail, if we inc first, the info may be overwritten
        cqes[filled].index = cqe.index;
        cqes[filled].result = cqe.result;
        cqes[filled].userdata = cqe.userdata;

        // then we inc the tail, to make sure we're the only one taking it as output
        if (!ring.cqeTail.compare_exchange_strong(
                t,
                (t + 1) % ring.entries)) {  // another thread is also popping out cqe, we'll yield
          break;
        }

        // if we own the cqe, we can add it to our output
        // if inc'ing cqe tail failed in the prev step, discard the recorded cqe info by not inc'ing filled
        ++filled;
        ring.slots.dealloc(cqe.index);
      }

      // post sem to signal the available slots in cqe section
      hf3fs_submit_ios(ior);
      continue;
    }

    if (filled >= min_results) {  // no more immediate results and we've got enough
      return filled;
    }

    struct timespec start;
    if (clock_gettime(CLOCK_REALTIME, &start) < 0) {
      continue;
    }
    auto ts = start;

    if (abs_timeout &&
        (abs_timeout->tv_sec < ts.tv_sec ||
         (abs_timeout->tv_sec == ts.tv_sec && abs_timeout->tv_nsec <= ts.tv_nsec))) {  // already timed out
      return filled;
    }

    auto nsec = ts.tv_nsec + jitter * 1000000;
    ts.tv_nsec = nsec % 1000000000;
    ts.tv_sec = ts.tv_sec + nsec / 1000000000;
    if (abs_timeout && ts.tv_sec >= abs_timeout->tv_sec) {
      ts.tv_sec = abs_timeout->tv_sec;
      ts.tv_nsec = std::min(ts.tv_nsec, abs_timeout->tv_nsec);
    }

    // wait for cqe sem, don't care if it succeeds, times out, or even fails
    // we check the cqe section again in any case
    sem_timedwait(ring.cqeSem.get(), &ts);
  } while (filled < cqec);

  return filled;
}

int hf3fs_hardlink(const char *target, const char *link_name) {
  int fd = open(target, O_RDONLY);
  if (fd == -1) {
    return errno;
  }
  SCOPE_EXIT { close(fd); };
  std::filesystem::path link_path(link_name);
  hf3fs::lib::fuse::Hf3fsIoctlHardlinkArg arg;
  struct stat buf;
  auto res = stat(link_path.parent_path().c_str(), &buf);
  if (res != 0) {
    return errno;
  }
  arg.ino = buf.st_ino;
  strcpy(arg.str, link_path.filename().c_str());
  res = ioctl(fd, hf3fs::lib::fuse::HF3FS_IOC_HARDLINK, &arg);
  if (res != 0) {
    return errno;
  }
  return 0;
}

int hf3fs_punchhole(int fd, int n, const size_t *start, const size_t *end, size_t flags) {
  hf3fs::lib::fuse::Hf3fsIoctlPunchHoleArg arg;
  arg.n = n;
  arg.flags = flags;
  for (int i = 0; i < n; i++) {
    arg.start[i] = start[i];
    arg.end[i] = end[i];
  }
  auto res = ioctl(fd, hf3fs::lib::fuse::HF3FS_IOC_PUNCH_HOLE, &arg);
  if (res != 0) {
    return errno;
  }
  return 0;
}
