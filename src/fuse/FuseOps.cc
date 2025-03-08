#include "FuseOps.h"

#include <algorithm>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <fuse3/fuse_lowlevel.h>
#include <iostream>
#include <linux/fs.h>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <sys/types.h>
#include <utility>

#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Path.h"
#include "common/utils/RequestInfo.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "common/utils/VersionInfo.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fmt/core.h"
#include "fmt/format.h"
#include "fuse/IoRing.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#pragma clang diagnostic ignored "-Wflexible-array-extensions"
#include <fuse3/fuse.h>
#pragma clang diagnostic pop
#include "lib/api/fuse.h"
#include "lib/api/hf3fs.h"

namespace hf3fs::fuse {
FuseClients d;
monitor::CountRecorder fuseOp{"fuse.op"};
void record(std::string_view op, uid_t uid) {
  fuseOp.addSample(1, {{"instance", std::string(op)}, {"uid", folly::to<std::string>(uid)}});
}

FuseClients &getFuseClientsInstance() { return d; }

namespace {
class RequestInfo : public hf3fs::RequestInfo, folly::NonCopyableNonMovable {
 public:
  RequestInfo(fuse_req_t req)
      : req_(req) {}

  static auto set(fuse_req_t req) {
    return folly::ShallowCopyRequestContextScopeGuard(hf3fs::RequestInfo::token(), std::make_unique<RequestInfo>(req));
  }

  std::string describe() const override { return fmt::format("{}@fuse", fuse_req_ctx(req_)->pid); }
  bool canceled() const override {
    if (!d.config->enable_interrupt()) {
      return false;
    }
    return fuse_req_interrupted(req_);
  }

 private:
  fuse_req_t req_;
};

template <typename Awaitable>
auto withRequestInfo(fuse_req_t req, Awaitable &&awaitable) {
  auto guard = RequestInfo::set(req);
  return folly::coro::blockingWait(std::forward<Awaitable>(awaitable));
}

std::string proc_cmdline(pid_t pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline");
  std::stringstream buffer;
  buffer << f.rdbuf();
  auto name = buffer.str();
  for (auto &c : name) {
    if (c == 0) {
      c = ' ';
    }
  }
  return name;
}

bool check_rmrf(pid_t pid) {
  // if user is trying to recursive remove with ln -s,
  // ensure that there are no extra spaces in the path entered by the user.
  std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline");
  std::stringstream buffer;
  buffer << f.rdbuf();
  auto cmdline = buffer.str();
  auto args = std::vector<std::string>();
  folly::split('\0', cmdline, args);
  auto bin = args[0];
  auto ln = std::set<std::string>{"ln", "/usr/bin/ln", "/bin/ln"};

  if (ln.contains(bin)) {
    bool doubleDash = false;
    bool skip = false;
    for (size_t pos = 1; pos < args.size(); pos++) {
      auto arg = args[pos];
      if (arg.empty() || std::all_of(arg.begin(), arg.end(), [](auto c) { return c == ' '; })) {
        continue;
      }

      if (arg.starts_with("-") && !doubleDash) {
        if (arg == "--") {
          doubleDash = true;
          skip = false;
        } else {
          skip = true;
        }
        continue;
      }

      if (skip) {
        skip = false;
        continue;
      }

      auto path = Path(arg).lexically_normal();
      if (!path.is_absolute()) {
        XLOGF(CRITICAL,
              "Recursive remove with invalid args, cmdline = '{}', path '{}' is not absolute",
              fmt::join(args.begin(), args.end(), " "),
              path);
        return false;
      }

      static const auto mountPoint = d.fuseMountpoint.lexically_normal();
      static const auto remountPref =
          d.fuseRemountPref ? std::make_optional(d.fuseRemountPref->lexically_normal()) : std::nullopt;
      auto [mpm, pm] = std::mismatch(mountPoint.begin(), mountPoint.end(), path.begin(), path.end());
      auto is3fs = mpm == mountPoint.end();
      if (!is3fs && remountPref) {
        auto [mpm2, pm2] = std::mismatch(remountPref->begin(), remountPref->end(), path.begin(), path.end());
        if (mpm2 == remountPref->end()) {
          pm = pm2;
          is3fs = true;
        }
      }
      if (!is3fs) {
        XLOGF(CRITICAL,
              "Recursive remove with invalid args, cmdline = '{}', path '{}' not on this 3fs, mount {}, remount {}",
              fmt::join(args.begin(), args.end(), " "),
              path,
              mountPoint,
              OptionalFmt(remountPref));
        return false;
      }
    }
  }
  return true;
}

InodeId real_ino(fuse_ino_t ino) {
  if (ino == FUSE_ROOT_ID) {
    return InodeId::root();
  }
  if (ino == FUSE_ROOT_ID + 1) {
    return InodeId::gcRoot();
  }
  return InodeId(ino);
}

fuse_ino_t linux_ino(InodeId ino) {
  if (ino == InodeId::root()) {
    return FUSE_ROOT_ID;
  }
  if (ino == InodeId::gcRoot()) {
    return FUSE_ROOT_ID + 1;
  }
  return ino.u64();
}

void setTime(timespec &t, const UtcTime &ut) {
  auto us = ut.toMicroseconds();
  t.tv_sec = us / 1'000'000;
  t.tv_nsec = us % 1'000'000 * 1000;
}

void fillLinuxStat(struct ::stat &statbuf, const Inode &inode) {
  statbuf.st_dev = statbuf.st_rdev = 0;
  statbuf.st_ino = linux_ino(inode.id);

  statbuf.st_blksize = inode.isSymlink()
                           ? 0
                           : (inode.isDirectory() ? inode.asDirectory().layout : inode.asFile().layout).chunkSize.u32();

  auto type = inode.isFile() ? S_IFREG : inode.isDirectory() ? S_IFDIR : inode.isSymlink() ? S_IFLNK : 0;
  XLOGF_IF(FATAL, !type, "Invalid inode type, {}", inode);

  statbuf.st_mode = (inode.acl.perm.toUnderType() & ALLPERMS) | type;
  statbuf.st_nlink = inode.nlink;
  statbuf.st_uid = inode.acl.uid.toUnderType();
  statbuf.st_gid = inode.acl.gid.toUnderType();
  statbuf.st_size = inode.isFile()      ? inode.asFile().length
                    : inode.isSymlink() ? inode.asSymlink().target.native().size()
                                        : 0;
  statbuf.st_blocks = inode.isFile() ? (statbuf.st_size + 511) / 512 : 0;  // we don't allow holes
  setTime(statbuf.st_atim, inode.atime);
  setTime(statbuf.st_mtim, inode.mtime);
  setTime(statbuf.st_ctim, inode.ctime);
}

void init_entry(struct fuse_entry_param *e, double attr_timeout, double entry_timeout) {
  memset(e, 0, sizeof(*e));
  e->attr_timeout = attr_timeout;
  e->entry_timeout = entry_timeout;
}

void add_entry(const Inode &inode, struct fuse_entry_param *e) {
  if (e) {
    fillLinuxStat(e->attr, inode);
    e->ino = e->attr.st_ino;
  }

  std::lock_guard lock{d.inodesMutex};
  auto it = d.inodes.find(inode.id);
  if (it != d.inodes.end()) {
    auto &rcinode = it->second;
    //    rcinode->inode = inode;
    rcinode->refcount++;
    rcinode->update(inode);
  } else {
    d.inodes.insert({inode.id, std::make_shared<RcInode>(inode, 1)});
  }
}

void add_empty_entry(const InodeId &inodeid) {
  std::lock_guard lock{d.inodesMutex};
  auto it = d.inodes.find(inodeid);
  if (it != d.inodes.end()) {
    auto &rcinode = it->second;
    rcinode->refcount++;
  } else {
    auto inode = Inode{};
    inode.id = inodeid;
    d.inodes.insert({inodeid, std::make_shared<RcInode>(inode, 1)});
  }
}

bool checkIsVirt(InodeId ino) { return (ino.u64() & (0xf000000000000000)) != 0; }

void remove_entry(InodeId ino, int n) {
  std::lock_guard lock{d.inodesMutex};
  auto it = d.inodes.find(ino);
  if (it == d.inodes.end()) {
    if (!checkIsVirt(ino)) {
      XLOGF(ERR, "remove_entry(ino={}): inode not found in list.", ino);
    }
  } else {
    auto &rcinode = it->second;
    if (rcinode->refcount >= n) {
      rcinode->refcount -= n;
      if (rcinode->refcount == 0) {
        d.inodes.erase(it);
      }
    } else {
      XLOGF(ERR, "remove_entry(ino={}): inode refcount less than {}.", ino, n);
    }
  }
}

template <typename T>
void handle_error(fuse_req_t req, const hf3fs::Result<T> &ret, bool entry = false) {
  if (ret.hasError()) {
    if (ret.error().code() != MetaCode::kNotFound) {
      XLOGF(INFO,
            "  handle_error({}, {}, pid={}, cmdline={})",
            ret.error().code(),
            ret.error().message(),
            fuse_req_ctx(req)->pid,
            proc_cmdline(fuse_req_ctx(req)->pid));
    } else {
      XLOGF(OP_LOG_LEVEL,
            "  handle_error({}, {}, pid={})",
            ret.error().code(),
            ret.error().message(),
            fuse_req_ctx(req)->pid);
    }
    int err = StatusCode::toErrno(ret.error().code());
    auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
    if (entry && ret.error().code() == MetaCode::kNotFound &&
        d.userConfig.getConfig(userInfo).negative_timeout() != 0) {
      fuse_entry_param e;
      memset(&e, 0, sizeof(e));
      e.entry_timeout = d.userConfig.getConfig(userInfo).negative_timeout();
      fuse_reply_entry(req, &e);
    } else {
      fuse_reply_err(req, err);
    }
  }
}

void notify_inval_inode(InodeId inodeId) { fuse_lowlevel_notify_inval_inode(d.se, linux_ino(inodeId), -1, 0); }

void notify_inval_entry(InodeId parent, std::string name) {
  fuse_lowlevel_notify_inval_entry(d.se, linux_ino(parent), name.c_str(), name.size());
}

void hf3fs_init(void *userdata, struct fuse_conn_info *conn) {
  (void)userdata;

  XLOGF(INFO, "hf3fs_init()");
  if (d.enableWritebackCache && (conn->capable & FUSE_CAP_WRITEBACK_CACHE)) {
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
    XLOGF(INFO, "FUSE writeback cache: ON");
  }
  if (conn->capable & FUSE_CAP_SPLICE_WRITE) {
    conn->want |= FUSE_CAP_SPLICE_WRITE;
    XLOGF(INFO, "FUSE splice write: ON");
  }
  if (conn->capable & FUSE_CAP_SPLICE_READ) {
    conn->want |= FUSE_CAP_SPLICE_READ;
    XLOGF(INFO, "FUSE splice read: ON");
  }
  if (conn->capable & FUSE_CAP_SPLICE_MOVE) {
    conn->want |= FUSE_CAP_SPLICE_MOVE;
    XLOGF(INFO, "FUSE splice move: ON");
  }

  d.maxBufsize = std::min(d.config->max_readahead(), d.config->io_bufs().max_buf_size());
  conn->max_readahead = d.config->io_bufs().max_buf_size();
  conn->max_read = d.config->io_bufs().max_buf_size();
  conn->max_write = d.config->io_bufs().max_buf_size();
  conn->max_background = d.config->max_background();
  conn->time_gran = d.config->time_granularity().asUs().count() * 1000;
}

void hf3fs_destroy(void *userdata) { (void)userdata; }

// // 2023/6/1 as virtual inode timestamps
// const auto virtInodeTime = UtcTime(std::chrono::microseconds(1685548800ull * 1000 * 1000));

// /3fs-virt dir to contain all virtual dirs and files
const std::string virtDir = "3fs-virt";
const auto virtDirInode =
    Inode{InodeId::virt(), InodeData{Directory{InodeId{InodeId::root()}}, Acl{Uid{0}, Gid{0}, Permission{0555}}}};
// virtInodeTime,
// virtInodeTime,
// virtInodeTime}};

struct NameInode {
  std::string name;
  Inode inode;
};

const std::vector<NameInode> topVirtDirs{
    // /3fs-virt/rm-rf for removing dirs directly without removing dir entries first
    {"rm-rf",
     Inode{InodeId::rmRf(), InodeData{Directory{InodeId{virtDirInode.id}}, Acl{Uid{0}, Gid{0}, Permission{0777}}}}},

    {"iovs",
     Inode{InodeId::iovDir(), InodeData{Directory{InodeId{virtDirInode.id}}, Acl{Uid{0}, Gid{0}, Permission{0777}}}}},
    {"get-conf",
     Inode{InodeId::getConf(), InodeData{Directory{InodeId{virtDirInode.id}}, Acl{Uid{0}, Gid{0}, Permission{0555}}}}},
    {"set-conf",
     Inode{InodeId::setConf(), InodeData{Directory{InodeId{virtDirInode.id}}, Acl{Uid{0}, Gid{0}, Permission{0777}}}}},
};

std::optional<std::string> checkVirtDir(InodeId ino, const Inode **inode = nullptr) {
  if (ino == virtDirInode.id) {  // virtual dir /3fs-virt
    if (inode) {
      *inode = &virtDirInode;
    }
    return virtDir;
  } else {
    for (const auto &ni : topVirtDirs) {
      if (ino == ni.inode.id) {
        if (inode) {
          *inode = &ni.inode;
        }
        return ni.name;
      }
    }
  }

  return std::nullopt;
}

void hf3fs_forget(fuse_req_t req, fuse_ino_t fino, uint64_t nlookup) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_forget(ino={}, nlookup={}, pid={})", ino, nlookup, fuse_req_ctx(req)->pid);
  record("forget", fuse_req_ctx(req)->uid);

  remove_entry(ino, nlookup);
  fuse_reply_none(req);
}

std::shared_ptr<RcInode> inodeOf(InodeId ino) {
  std::lock_guard lock{d.inodesMutex};
  auto it = d.inodes.find(ino);
  return it != d.inodes.end() ? it->second : std::shared_ptr<RcInode>();
}

std::shared_ptr<RcInode> inodeOf(struct fuse_file_info &fi, InodeId ino) {
  if (fi.fh && ((FileHandle *)fi.fh)->rcinode) {
    return ((FileHandle *)fi.fh)->rcinode;
  }
  return inodeOf(ino);
}

CoTryTask<ssize_t> flushBuf(const flat::UserInfo &user,
                            const std::shared_ptr<RcInode> &pi,
                            const off_t off,
                            storage::client::IOBuffer &memh,
                            const size_t len,
                            bool flushAll) {
  static auto mountName = d.fuseRemountPref.value_or(d.fuseMountpoint).native();

  static monitor::LatencyRecorder overallLatency("fuse.piov.overall.write", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder prepareLatency("fuse.piov.prepare.write", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder submitLatency("fuse.piov.submit.write", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder completeLatency("fuse.piov.complete.write",
                                                  monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ioSizeDist("fuse.piov.io_size.write",
                                                  monitor::TagSet{{"mount_name", mountName}});
  static monitor::CountRecorder bwCount("fuse.piov.bw", monitor::TagSet{{"mount_name", mountName}});

  std::vector<ssize_t> res(1);
  auto uids = std::to_string(user.uid);

  auto begin = co_await pi->beginWrite(user, *d.metaClient, off, len);
  CO_RETURN_ON_ERROR(begin);
  auto truncateVer = *begin;
  auto onError = folly::makeGuard([&]() { pi->finishWrite(user, truncateVer, off, -1); });

  size_t done = 0;
  do {
    auto start = SteadyClock::now(), overallStart = start;

    ioSizeDist.addSample(len - done, monitor::TagSet{{"uid", uids}});

    res[0] = 0;
    PioV ioExec(*d.storageClient, d.userConfig.getConfig(user).chunk_size_limit(), res);
    auto retAdd = ioExec.addWrite(0, pi->inode, 0, off + done, len - done, memh.data() + done, memh);
    CO_RETURN_ON_ERROR(retAdd);

    auto now = SteadyClock::now();
    prepareLatency.addSample(now - start, monitor::TagSet{{"uid", uids}});
    start = now;

    auto retExec = co_await ioExec.executeWrite(user, d.config->storage_io().write());
    now = SteadyClock::now();
    submitLatency.addSample(now - start, monitor::TagSet{{"uid", uids}});
    CO_RETURN_ON_ERROR(retExec);

    ioExec.finishIo(true);

    now = SteadyClock::now();
    completeLatency.addSample(now - start, monitor::TagSet{{"uid", uids}});
    overallLatency.addSample(now - overallStart, monitor::TagSet{{"uid", uids}});

    if (res[0] < 0) {
      XLOGF(ERR, "  hf3fs_write error, ino {}, size {}, off {}, err {}", pi->inode.id, len, off, res[0]);
      co_return makeError(-res[0], "hf3fs_write error");
    } else {
      XLOGF(DBG, "flushed {} bytes", res[0]);
      done += res[0];
    }

    bwCount.addSample(res[0], monitor::TagSet{{"uid", uids}});
  } while (flushAll && done < len);

  // success
  onError.dismiss();
  pi->finishWrite(user, truncateVer, off, done);

  co_return done;
}

ssize_t flushBuf(fuse_req_t req,
                 const std::shared_ptr<RcInode> &pi,
                 const off_t off,
                 storage::client::IOBuffer &memh,
                 const size_t len,
                 bool flushAll) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  auto result = withRequestInfo(req, flushBuf(userInfo, pi, off, memh, len, flushAll));
  if (result.hasError()) {
    handle_error(req, result);
    return -1;
  } else {
    return *result;
  }
}

enum class SyncType {
  Lookup,
  GetAttr,
  PeriodSync,
  Fsync,
  ForceFsync,
};

CoTryTask<std::optional<Inode>> sync(flat::UserInfo userInfo, RcInode &inode, SyncType type) {
  auto guard = inode.dynamicAttr.rlock();
  auto force = type == SyncType::ForceFsync;
  auto fsync = force || type == SyncType::Fsync;
  auto syncver = fsync ? guard->fsynced : guard->synced;
  auto writever = guard->written;
  auto atime = guard->atime;
  auto mtime = guard->mtime;
  if (!force && syncver >= writever) {
    XLOGF_IF(FATAL, syncver > writever, "{}, {} > {}", inode.inode.id, syncver, writever);
    co_return std::nullopt;
  }
  auto hint =
      (fsync && (force || !d.config->fsync_length_hint())) ? std::optional<meta::VersionedLength>() : guard->hintLength;
  guard.unlock();

  auto res = co_await d.metaClient->sync(userInfo, inode.inode.id, true, atime, mtime, hint);
  if (type != SyncType::GetAttr && type != SyncType::Lookup) {
    // NOTE: shouldn't call inval inode during get_attr or lookup, otherwise fuse may think the result of get_attr or
    // lookup is invalid.
    notify_inval_inode(inode.inode.id);
  }
  if (res.hasError()) {
    // sync failed
    inode.clearHintLength();
    CO_RETURN_ERROR(res);
  } else {
    // sync success
    XLOGF_IF(FATAL, !res->isFile(), "not file but sync success, {}", *res);
    inode.update(*res, writever, fsync);
    co_return std::move(*res);
  }
}

CoTryTask<void> close(flat::UserInfo userInfo, RcInode &inode, Uuid session) {
  auto guard = inode.dynamicAttr.rlock();
  auto syncver = guard->fsynced;
  auto writever = guard->written;
  auto updateLength = writever > syncver;
  auto atime = guard->atime;
  auto mtime = guard->mtime;
  guard.unlock();

  auto res = co_await d.metaClient->close(userInfo, inode.inode.id, session, updateLength, atime, mtime);
  if (updateLength) {
    notify_inval_inode(inode.inode.id);
  }
  if (res.hasError()) {
    inode.clearHintLength();
    CO_RETURN_ERROR(res);
  } else {
    if (updateLength) {
      inode.update(*res, writever, true);
    } else {
      inode.update(*res);
    }
  }

  co_return Void{};
}

CoTryTask<Inode> truncate(flat::UserInfo userInfo, RcInode &inode, size_t length) {
  auto writever = inode.dynamicAttr.rlock()->written;

  auto res = co_await d.metaClient->truncate(userInfo, inode.inode.id, length);
  if (res.hasError()) {
    // truncate failed
    inode.clearHintLength();
  } else {
    // truncate success
    XLOGF_IF(FATAL, !res->isFile(), "not file but truncate success, {}", *res);
    inode.update(*res, writever, true);
  }
  co_return res;
}

bool flushAndSync(fuse_req_t req,
                  fuse_ino_t fino,
                  int flushOnly,
                  SyncType syncType,
                  struct fuse_file_info *fi,
                  struct fuse_entry_param *e = nullptr) {
  auto ino = real_ino(fino);

  struct fuse_file_info fi2 {};
  if (!fi) {
    fi = &fi2;
  }

  auto pi = inodeOf(*fi, ino);
  if (!pi || !pi->inode.isFile()) {
    return true;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  auto issync = syncType == SyncType::Fsync || syncType == SyncType::ForceFsync || syncType == SyncType::PeriodSync;
  if (issync || d.userConfig.getConfig(userInfo).flush_on_stat()) {
    std::lock_guard lock(pi->wbMtx);
    auto wb = pi->writeBuf;
    if (wb) {
      if (wb->len && flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
        XLOGF(DBG, "flush buf failed");
        wb->len = 0;
        return false;
      }
      wb->len = 0;
    }
  }

  if (flushOnly) {  // no need to recalc file size on meta side
    XLOGF(DBG, "flush only, no need to sync");
    return true;
  }

  d.dirtyInodes.lock()->erase(ino);
  auto res = withRequestInfo(req, sync(userInfo, *pi, syncType));
  if (res.hasError()) {
    handle_error(req, res);
    return false;
  } else {
    if (res->has_value() && e) {
      fillLinuxStat(e->attr, **res);
    }
  }
  return true;
}

void hf3fs_lookup(fuse_req_t req, fuse_ino_t fparent, const char *name) {
  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_lookup(parent={}, name={}, pid={})", parent, name, fuse_req_ctx(req)->pid);
  record("lookup", fuse_req_ctx(req)->uid);

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  struct fuse_entry_param e;
  init_entry(&e, d.userConfig.getConfig(userInfo).attr_timeout(), d.userConfig.getConfig(userInfo).entry_timeout());
  if (parent == InodeId::root() && name == virtDir) {  // virtual dir /3fs-virt
    fillLinuxStat(e.attr, virtDirInode);
    e.ino = linux_ino(virtDirInode.id);
    fuse_reply_entry(req, &e);
    return;
  } else {
    auto dname = checkVirtDir(parent);
    if (dname) {
      if (*dname == virtDir) {
        for (const auto &ni : topVirtDirs) {
          if (ni.name == name) {
            fillLinuxStat(e.attr, ni.inode);
            e.ino = linux_ino(ni.inode.id);
            fuse_reply_entry(req, &e);
            return;
          }
        }

        //    std::cout << "ino " << parent << " name " << name << std::endl;
        fuse_reply_err(req, ENOENT);
        return;
      } else if (*dname == "iovs") {
        for (int prio = 0; prio <= 2; ++prio) {
          if (d.iors.semName(prio) == name) {
            fillLinuxStat(e.attr, d.iors.lookupSem(prio));
            e.ino = e.attr.st_ino;
            fuse_reply_entry(req, &e);
            return;
          }
        }

        auto res = d.iovs.lookupIov(name, userInfo);
        if (res.hasError()) {
          handle_error(req, res);
          return;
        } else {
          fillLinuxStat(e.attr, *res);
          e.ino = e.attr.st_ino;
          fuse_reply_entry(req, &e);
          return;
        }
      } else if (*dname == "get-conf") {
        auto res = d.userConfig.lookupConfig(name, userInfo);
        if (res.hasError()) {
          handle_error(req, res);
          return;
        } else {
          XLOGF(DBG, "inode id {} uid {}", res->id, res->data().acl.uid);
          fillLinuxStat(e.attr, *res);
          e.ino = e.attr.st_ino;
          fuse_reply_entry(req, &e);
          return;
        }
      } else {
        fuse_reply_err(req, ENOENT);
        return;
      }
    }
  }

  auto res = withRequestInfo(req, d.metaClient->stat(userInfo, parent, name, false));
  if (res.hasError()) {
    handle_error(req, res, true);
  } else {
    if (res->isSymlink()) {
      e.attr_timeout = d.userConfig.getConfig(userInfo).symlink_timeout();
      e.entry_timeout = d.userConfig.getConfig(userInfo).symlink_timeout();
    }
    add_entry(res.value(), &e);
    if (d.userConfig.getConfig(userInfo).sync_on_stat()) {
      if (!flushAndSync(req, linux_ino(res.value().id), false /* flushOnly */, SyncType::Lookup, nullptr, &e)) {
        return;
      }
    }
    fuse_reply_entry(req, &e);
  }
}

void hf3fs_getattr(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_getattr(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("getattr", fuse_req_ctx(req)->uid);

  struct ::stat buf;

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  const Inode *pino;
  if (checkVirtDir(ino, &pino)) {
    fillLinuxStat(buf, *pino);
    fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
    return;
  } else {
    for (int prio = 0; prio <= 2; ++prio) {
      if (ino == d.iors.lookupSem(prio).id) {
        fillLinuxStat(buf, d.iors.lookupSem(prio));
        fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
        return;
      }
    }

    auto iovRes = d.iovs.iovDesc(ino);
    if (iovRes) {
      auto res = d.iovs.statIov(*iovRes, userInfo);
      if (!res) {
        handle_error(req, res);
      } else {
        fillLinuxStat(buf, *res);
        fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
      }
      return;
    }

    auto confRes = d.userConfig.statConfig(ino, userInfo);
    if (confRes) {
      XLOGF(DBG, "inode id {} uid {}", confRes->id, confRes->data().acl.uid);
      fillLinuxStat(buf, *confRes);
      fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
      return;
    }
  }

  if (d.userConfig.getConfig(userInfo).sync_on_stat() &&
      !flushAndSync(req, fino, false /* flushOnly */, SyncType::GetAttr, fi)) {
    return;
  }

  // fuse_file_info fi2{};
  // auto ptr = inodeOf(fi2, ino);

  auto res = withRequestInfo(
      req,
      d.metaClient->stat(userInfo, ino, std::nullopt, false));  // [&userInfo, &ptr, ino]() -> CoTryTask<Inode> {
  //   auto syncver = ptr->synced.load();
  //   auto writever = ptr->written.load();
  //   if (syncver < writever) {
  //     CO_RETURN_ON_ERROR(co_await d.metaClient->sync(userInfo, ino, true, true));
  //     ptr->synced.store(writever);
  //   }

  //   co_return co_await d.metaClient->stat(userInfo, ino, std::nullopt, false);
  // }());
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    fillLinuxStat(buf, res.value());
    fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
    if (auto pi = inodeOf(ino); pi) {
      pi->update(*res);
    }
  }
}

void hf3fs_setattr(fuse_req_t req, fuse_ino_t fino, struct stat *attr, int to_set, struct fuse_file_info *fi) {
  (void)fi;

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto ino = real_ino(fino);
  struct fuse_file_info fi2 {};
  if (!fi) {
    fi = &fi2;
  }
  auto pi = inodeOf(*fi, ino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_setattr(ino={}, to_set={}, pid={})", ino, to_set, fuse_req_ctx(req)->pid);
  record("setattr", fuse_req_ctx(req)->uid);

  if (checkVirtDir(ino)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  std::optional<Result<Inode>> res;
  if (to_set & (FUSE_SET_ATTR_MODE | FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
    std::optional<Permission> perm = std::nullopt;
    std::optional<Uid> uid = std::nullopt;
    std::optional<Gid> gid = std::nullopt;
    if (to_set & FUSE_SET_ATTR_UID) {
      uid = Uid(attr->st_uid);
    }
    if (to_set & FUSE_SET_ATTR_GID) {
      gid = Gid(attr->st_gid);
    }
    if (to_set & FUSE_SET_ATTR_MODE) {
      perm = Permission(attr->st_mode & ALLPERMS);
    }
    res = withRequestInfo(req, d.metaClient->setPermission(userInfo, ino, std::nullopt, false, uid, gid, perm));
    if (res->hasError()) {
      handle_error(req, res.value());
      return;
    }
  }
  if (to_set & FUSE_SET_ATTR_SIZE) {
    {
      std::lock_guard lock(pi->wbMtx);
      auto wb = pi->writeBuf;
      if (wb) {
        if (wb->len) {
          XLOGF(DBG, "flushing for truncate");
          if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
            wb->len = 0;
            return;
          }
          wb->len = 0;
        }
      }
    }
    res = withRequestInfo(req, truncate(userInfo, *pi, attr->st_size));
    if (res->hasError()) {
      handle_error(req, res.value());
      return;
    }
  }
  if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
    std::optional<UtcTime> atime, mtime;
    if (to_set & FUSE_SET_ATTR_ATIME) {
      pi->dynamicAttr.wlock()->atime.reset();
      if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
        atime = SETATTR_TIME_NOW;
      } else {
        atime = UtcTime::from(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds{attr->st_atim.tv_sec} + std::chrono::nanoseconds{attr->st_atim.tv_nsec}));
      }
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
      pi->dynamicAttr.wlock()->mtime.reset();
      if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
        mtime = SETATTR_TIME_NOW;
      } else {
        mtime = UtcTime::from(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds{attr->st_mtim.tv_sec} + std::chrono::nanoseconds{attr->st_mtim.tv_nsec}));
      }
    }
    res = withRequestInfo(req, d.metaClient->utimes(userInfo, ino, std::nullopt, false, atime, mtime));
    if (res->hasError()) {
      handle_error(req, res.value());
      return;
    }
  }

  if (res) {
    struct ::stat buf;
    fillLinuxStat(buf, res->value());
    fuse_reply_attr(req, &buf, d.userConfig.getConfig(userInfo).attr_timeout());
  } else {
    hf3fs_getattr(req, linux_ino(ino), fi);
  }
}

void hf3fs_readlink(fuse_req_t req, fuse_ino_t fino) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_readlink(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("readlink", fuse_req_ctx(req)->uid);

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  for (int prio = 0; prio <= 2; ++prio) {
    auto inode = d.iors.lookupSem(prio);
    if (ino == inode.id) {
      fuse_reply_readlink(req, inode.data().asSymlink().target.c_str());
      return;
    }
  }

  if (checkVirtDir(ino)) {
    fuse_reply_err(req, EINVAL);
    return;
  } else {
    auto iovRes = d.iovs.iovDesc(ino);
    if (iovRes) {
      auto res = d.iovs.statIov(*iovRes, userInfo);
      if (res) {
        fuse_reply_readlink(req, res->data().asSymlink().target.c_str());
      } else {
        handle_error(req, res);
      }
      return;
    }

    auto confRes = d.userConfig.statConfig(ino, userInfo);
    if (confRes) {
      XLOGF(DBG, "inode id {} uid {}", confRes->id, confRes->data().acl.uid);
      fuse_reply_readlink(req, confRes->data().asSymlink().target.c_str());
      return;
    }
  }

  /*
  auto res = folly::coro::blocking_wait(d.metaClient->stat(userInfo, ino, std::nullopt, false));
  if (res.hasError()) {
    if (res.error().code() == MetaCode::kNotFound) {
      fuse_lowlevel_notify_inval_inode(d.se, ino, 0, 0);
    }
  }
  */

  fuse_file_info fi{};
  auto ptr = inodeOf(fi, ino);
  if (!ptr) {
    XLOGF(ERR, "Inode {} not found", ino);
    fuse_reply_err(req, ENOENT);
    return;
  }
  if (ptr->inode.isSymlink()) {
    fuse_reply_readlink(req, ptr->inode.asSymlink().target.c_str());
  } else {
    fuse_reply_err(req, EINVAL);
  }
}

void hf3fs_mknod(fuse_req_t req, fuse_ino_t fparent, const char *name, mode_t mode, dev_t rdev) {
  (void)rdev;

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_mknod(parent={}, name={}, mode={}, pid={})", parent, name, mode, fuse_req_ctx(req)->pid);
  record("mknod", fuse_req_ctx(req)->uid);

  if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode)) {
    fuse_reply_err(req, ENOSYS);
    return;
  }

  if (checkVirtDir(parent)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  auto res = withRequestInfo(
      req,
      d.metaClient->create(userInfo, parent, name, std::nullopt, meta::Permission(mode & ALLPERMS), O_RDONLY));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    struct fuse_entry_param e;
    init_entry(&e, d.userConfig.getConfig(userInfo).attr_timeout(), d.userConfig.getConfig(userInfo).entry_timeout());
    add_entry(res.value(), &e);
    fuse_reply_entry(req, &e);
  }
}

void hf3fs_mkdir(fuse_req_t req, fuse_ino_t fparent, const char *name, mode_t mode) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_mkdir(parent={}, name={}, mode={}, pid={})", parent, name, mode, fuse_req_ctx(req)->pid);
  record("mkdir", fuse_req_ctx(req)->uid);

  auto dname = checkVirtDir(parent);
  if (dname) {
    //    std::cout << "mkdir in virt dir " << *dname << std::endl;
    fuse_reply_err(req, EPERM);
    return;
  }

  auto res =
      withRequestInfo(req, d.metaClient->mkdirs(userInfo, parent, name, meta::Permission(mode & ALLPERMS), false));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    struct fuse_entry_param e;
    init_entry(&e, d.userConfig.getConfig(userInfo).attr_timeout(), d.userConfig.getConfig(userInfo).entry_timeout());
    add_entry(res.value(), &e);
    fuse_reply_entry(req, &e);
  }
}

void hf3fs_unlink(fuse_req_t req, fuse_ino_t fparent, const char *name) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_unlink(parent={}, name={}, pid={})", parent, name, fuse_req_ctx(req)->pid);
  record("unlink", fuse_req_ctx(req)->uid);

  auto dname = checkVirtDir(parent);
  if (dname) {
    if (*dname == "iovs") {
      for (int prio = 0; prio <= 2; ++prio) {
        if (d.iors.semName(prio) == name) {
          fuse_reply_err(req, EPERM);
          return;
        }
      }

      auto res = d.iovs.rmIov(name, userInfo);
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        if (*res && (*res)->isIoRing) {
          d.iors.rmIoRing((*res)->iorIndex);
        }
        fuse_reply_err(req, 0);
      }
    } else {
      fuse_reply_err(req, EPERM);
    }

    return;
  }

  auto res = withRequestInfo(req, d.metaClient->remove(userInfo, parent, name, false));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    fuse_reply_err(req, 0);
  }
}

void hf3fs_rmdir(fuse_req_t req, fuse_ino_t fparent, const char *name) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_rmdir(parent={}, name={}, pid={})", parent, name, fuse_req_ctx(req)->pid);
  record("rmdir", fuse_req_ctx(req)->uid);

  if (checkVirtDir(parent)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  auto res = withRequestInfo(req, d.metaClient->remove(userInfo, parent, name, false));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    fuse_reply_err(req, 0);
  }
}

Result<NameInode> extractServerDirPath(fuse_req_t req, const char *link, const UserInfo &userInfo) {
  static const auto mountPoint = d.fuseMountpoint.lexically_normal();
  static const auto remountPref =
      d.fuseRemountPref ? std::make_optional(d.fuseRemountPref->lexically_normal()) : std::nullopt;

  auto p = Path(link).lexically_normal();
  if (p.is_relative()) {
    // can only link to absolute paths starting with our own mount point
    return makeError(StatusCode::kInvalidArg, "link is not absolute");
  }

  auto [mpm, pm] = std::mismatch(mountPoint.begin(), mountPoint.end(), p.begin(), p.end());
  auto is3fs = mpm == mountPoint.end();
  if (!is3fs && remountPref) {
    auto [mpm2, pm2] = std::mismatch(remountPref->begin(), remountPref->end(), p.begin(), p.end());
    if (mpm2 == remountPref->end()) {
      pm = pm2;
      is3fs = true;
    }
  }

  if (!is3fs) {
    return makeError(StatusCode::kInvalidArg,
                     fmt::format("link '{}' is not on this mount of 3fs, mount point '{}' remount prefix '{}'",
                                 link,
                                 mountPoint,
                                 remountPref.value_or("")));
  }

  // auto svrp = Path(pm, p.end());
  auto svrp = Path("/");
  for (auto it = pm; it != p.end(); ++it) {
    svrp /= *it;
  }

  auto fn = svrp.filename();
  auto svrd = svrp.parent_path();
  if (svrp.filename_is_dot()) {
    fn = svrd.filename();
    svrd = svrd.parent_path();
  }

  auto res = withRequestInfo(req, [&userInfo, &svrd]() -> CoTryTask<Inode> {
    co_return co_await d.metaClient->stat(userInfo, meta::InodeId::root(), svrd, false);
  }());
  RETURN_ON_ERROR(res);

  return NameInode{fn.native(), *res};
}

void hf3fs_symlink(fuse_req_t req, const char *link, fuse_ino_t fparent, const char *name) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  auto parent = real_ino(fparent);
  auto pid = fuse_req_ctx(req)->pid;

  XLOGF(OP_LOG_LEVEL, "hf3fs_symlink(link={}, parent={}, name={}, pid={})", link, parent, name, pid);
  record("symlink", fuse_req_ctx(req)->uid);

  static const auto mountPoint = d.fuseMountpoint.lexically_normal();
  static const auto remountPref =
      d.fuseRemountPref ? std::make_optional(d.fuseRemountPref->lexically_normal()) : std::nullopt;

  struct fuse_entry_param e;
  init_entry(&e,
             d.userConfig.getConfig(userInfo).symlink_timeout(),
             d.userConfig.getConfig(userInfo).symlink_timeout());

  auto dname = checkVirtDir(parent);
  if (dname) {
    if (*dname == "rm-rf") {
      if (d.userConfig.getConfig(userInfo).readonly()) {
        fuse_reply_err(req, EROFS);
        return;
      }

      if (d.config->check_rmrf() && !check_rmrf(fuse_req_ctx(req)->pid)) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      XLOGF(INFO,
            "Try to recursive remove {}, uid {}, cmdline {}",
            link,
            fuse_req_ctx(req)->uid,
            proc_cmdline(fuse_req_ctx(req)->pid));

      auto res = extractServerDirPath(req, link, userInfo);
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        auto res2 = withRequestInfo(req, d.metaClient->remove(userInfo, res->inode.id, res->name, true));
        if (res2.hasError()) {
          handle_error(req, res2);
        } else {
          //          add_entry(Inode{InodeId{-(100ull << 30) - res->inode.id.u64()}, InodeData{meta::Symlink{link}}},
          //          &e);
          fillLinuxStat(e.attr, Inode{InodeId::virtTemporary(res->inode.id), InodeData{meta::Symlink{link}}});
          e.ino = e.attr.st_ino;

          // at most 1 thread may wait on lock
          notify_inval_entry(res->inode.id, res->name);
          fuse_reply_entry(req, &e);
          d.notifyInvalExec->add([parent, name = std::string(name)]() { notify_inval_entry(parent, name); });
        }
      }
    } else if (*dname == "iovs") {
      auto res = d.iovs.addIov(name,
                               Path(link),
                               pid,
                               userInfo,
                               &d.client->tpg().bgThreadPool().randomPick(),
                               *d.storageClient);
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        auto &[inode, ior] = *res;
        if (ior) {
          auto res2 = d.iors.addIoRing(d.fuseRemountPref.value_or(d.fuseMountpoint),
                                       res->second,
                                       name,
                                       userInfo,
                                       ior->forRead,
                                       ior->bufStart,
                                       ior->size,
                                       ior->ioDepth,
                                       *ior->iora);
          if (!res2) {
            handle_error(req, res);
          }
          // record the ior index for later removal
          res->second->iorIndex = *res2;
        }
        //        add_entry(inode, &e);
        fillLinuxStat(e.attr, inode);
        e.ino = e.attr.st_ino;
        fuse_reply_entry(req, &e);
      }
    } else if (*dname == "set-conf") {
      auto res = d.userConfig.setConfig(name, link, userInfo);
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        XLOGF(DBG, "done set config");
        fillLinuxStat(e.attr, *res);
        XLOGF(DBG, "filled linux stat");
        e.ino = e.attr.st_ino;
        fuse_reply_entry(req, &e);
        XLOGF(DBG, "replied entry");

        // // invalidate the link after replying, we don't actually want a link under the set conf dir
        // fuse_lowlevel_notify_inval_entry(d.se, e.ino, name, strlen(name));
        // fuse_lowlevel_notify_inval_inode(d.se, e.ino, -1, 0);
      }
    } else {
      fuse_reply_err(req, EPERM);
    }
    return;
  }

  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  static const std::string mvPref = "mv:";
  if (!strncmp(link, mvPref.c_str(), mvPref.size())) {
    auto rlink = link + mvPref.size();
    auto res = extractServerDirPath(req, rlink, userInfo);
    if (res.hasError()) {
      handle_error(req, res);
    } else {
      auto res2 = withRequestInfo(req, [&userInfo, &res, parent, name]() -> CoTryTask<Inode> {
        co_return co_await d.metaClient->rename(userInfo, res->inode.id, res->name, parent, name);
        //        co_return co_await (d.metaClient->stat(userInfo, parent, name, false));
      }());

      if (res2.hasError()) {
        handle_error(req, res2);
      } else {
        auto viid = InodeId::virtTemporary(res->inode.id);
        //        add_entry(Inode{viid, InodeData{meta::Symlink{rlink}}}, &e);
        // add_entry(*res2, &e);
        fillLinuxStat(e.attr, Inode{viid, InodeData{meta::Symlink{rlink}}});
        e.ino = e.attr.st_ino;

        d.notifyInvalExec->add([res, viid, parent, name = std::string(name), req, e]() {
          if (res->inode.id != parent) {
            notify_inval_entry(res->inode.id, res->name);
            fuse_reply_entry(req, &e);
          } else {
            fuse_reply_entry(req, &e);
            notify_inval_entry(res->inode.id, res->name);
          }
          notify_inval_entry(parent, name);
          notify_inval_inode(viid);
        });
      }
    }

    return;
  }

  auto res = withRequestInfo(req, d.metaClient->symlink(userInfo, parent, name, link));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    add_entry(res.value(), &e);
    fuse_reply_entry(req, &e);
  }
}

void hf3fs_rename(fuse_req_t req,
                  fuse_ino_t fparent,
                  const char *name,
                  fuse_ino_t fnewparent,
                  const char *newname,
                  unsigned int flags) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);
  auto newparent = real_ino(fnewparent);

  XLOGF(OP_LOG_LEVEL,
        "hf3fs_rename(parent={}, name={}, newparent={}, newname={}, flags={}, pid={})",
        parent,
        name,
        newparent,
        newname,
        flags,
        fuse_req_ctx(req)->pid);
  record("rename", fuse_req_ctx(req)->uid);

  if (checkVirtDir(parent)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  auto dname = checkVirtDir(newparent);
  if (dname) {
    if (*dname == "rm-rf") {
      auto res = withRequestInfo(req, d.metaClient->remove(userInfo, parent, name, true));
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        fuse_reply_err(req, 0);  // ENOENT);
        d.notifyInvalExec->add([newparent, name = std::string(name)]() { notify_inval_entry(newparent, name); });
      }
    } else {
      fuse_reply_err(req, EPERM);
    }
    return;
  }

  if (flags != 0) {
    fuse_reply_err(req, ENOSYS);
    return;
  }
  auto res = withRequestInfo(req, d.metaClient->rename(userInfo, parent, name, newparent, newname));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    fuse_reply_err(req, 0);
  }
}

void hf3fs_link(fuse_req_t req, fuse_ino_t fino, fuse_ino_t fnewparent, const char *newname) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto ino = real_ino(fino);
  auto newparent = real_ino(fnewparent);

  XLOGF(OP_LOG_LEVEL,
        "hf3fs_link(ino={}, newparent={}, newname={}, pid={})",
        ino,
        newparent,
        newname,
        fuse_req_ctx(req)->pid);
  record("link", fuse_req_ctx(req)->uid);

  if (checkVirtDir(ino)) {
    fuse_reply_err(req, EISDIR);
    return;
  }

  if (checkVirtDir(newparent)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  auto res = withRequestInfo(req, d.metaClient->hardLink(userInfo, ino, std::nullopt, newparent, Path(newname), false));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    struct fuse_entry_param e;
    init_entry(&e, d.userConfig.getConfig(userInfo).attr_timeout(), d.userConfig.getConfig(userInfo).entry_timeout());
    add_entry(res.value(), &e);
    fuse_reply_entry(req, &e);
  }
}

void hf3fs_open(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  if ((fi->flags & O_WRONLY) || (fi->flags & O_RDWR) || (fi->flags & O_CREAT) || (fi->flags & O_EXCL) ||
      (fi->flags & O_TRUNC) || (fi->flags & O_APPEND)) {
    if (d.userConfig.getConfig(userInfo).readonly()) {
      fuse_reply_err(req, EROFS);
      return;
    }
  }

  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_open(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("open", fuse_req_ctx(req)->uid);
  auto ptr = inodeOf(*fi, ino);

  if (!ptr) {
    XLOGF(ERR, "hf3fs_open(ino={}): inode not found in list.", ino);
    fuse_reply_err(req, ENOENT);
    return;
  }

  // if (ptr->opened.fetch_add(1) != 0) {
  //   auto res = withRequestInfo(req, d.metaClient->sync(userInfo, ino, true, std::nullopt,
  //   std::nullopt)); if (res.hasError()) {
  //     handle_error(req, res);
  //     return;
  //   }
  // }

  Uuid session;
  if ((fi->flags & O_ACCMODE) == O_WRONLY || (fi->flags & O_ACCMODE) == O_RDWR) {
    session = meta::client::SessionId::random();
    auto res = withRequestInfo(req, d.metaClient->open(userInfo, ino, std::nullopt, session, fi->flags));
    if (res.hasError()) {
      handle_error(req, res);
      return;
    }

    //    fi->direct_io = 1;
  }

  // O_DIRECT open means direc io
  // read cached disabled && not O_NONBLOCK open (for mmap) means direct io too
  fi->direct_io =
      (fi->flags & O_DIRECT) || (!d.userConfig.getConfig(userInfo).enable_read_cache() && !(fi->flags & O_NONBLOCK))
          ? 1
          : 0;

  XLOGF(DBG, "{}opened in o direct mode", fi->flags & O_DIRECT ? "" : "not ");
  fi->fh = (uintptr_t)(new FileHandle{ptr, (bool)(fi->flags & O_DIRECT), session});
  fuse_reply_open(req, fi);
}

void hf3fs_read(fuse_req_t req, fuse_ino_t fino, size_t size, off_t off, struct fuse_file_info *fi) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_read(ino={}, size={}, off={}, pid={})", ino, size, off, fuse_req_ctx(req)->pid);
  record("read", fuse_req_ctx(req)->uid);

  auto pi = inodeOf(*fi, ino);
  pi->dynamicAttr.wlock()->atime = UtcClock::now();

  std::unique_lock lock(pi->wbMtx);
  auto wb = pi->writeBuf;
  if (!wb) {
    lock.unlock();
  } else {
    if (wb->len) {
      XLOGF(DBG, "flushing for read");
      if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
        wb->len = 0;
        return;
      }
      wb->len = 0;
    }
  }

  auto &inode = pi->inode;
  /*
    if (!d.buf) {
      XLOGF(INFO, "  hf3fs_read register buffer");
      d.buf.reset(std::make_unique<std::vector<uint8_t>>(d.maxBufsize));
      auto ret = d.storageClient->registerIOBuffer(d.buf->data(), d.maxBufsize);
      if (ret.hasError()) {
        handle_error(req, ret);
        return;
      } else {
        d.memh.reset(std::make_unique<IOBuffer>(std::move(*ret)));
      }
    }
  */
  auto memh = IOBuffer(folly::coro::blocking_wait(d.bufPool->allocate()));

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  auto &config = d.userConfig.getConfig(userInfo);
  if (config.dryrun_bench_mode()) {
    fuse_reply_buf(req, (char *)memh.data(), size);
    return;
  }

  if (d.memsetBeforeRead) {
    memset(memh.data(), 0, size);
  }

  std::vector<ssize_t> res(1);
  PioV ioExec(*d.storageClient, config.chunk_size_limit(), res);
  auto retAdd = ioExec.addRead(0, inode, 0, off, size, memh.data(), memh);
  if (retAdd.hasError()) {
    handle_error(req, retAdd);
    return;
  }

  auto retExec = withRequestInfo(req, ioExec.executeRead(userInfo, d.config->storage_io().read()));
  if (retExec.hasError()) {
    handle_error(req, retExec);
    return;
  }

  ioExec.finishIo(true);

  if (lock.owns_lock()) {
    lock.unlock();
  }
  if (res[0] < 0) {
    XLOGF(ERR, "  hf3fs_read error, ino {}, size {}, off {}, err {}", ino, size, off, res[0]);
    fuse_reply_err(req, StatusCode::toErrno(-res[0]));
  } else {
    XLOGF(OP_LOG_LEVEL, "  read bytes, {}", res[0]);
    fuse_reply_buf(req, (char *)memh.data(), res[0]);
  }
}

void hf3fs_write(fuse_req_t req, fuse_ino_t fino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
  static auto mountName = d.fuseRemountPref.value_or(d.fuseMountpoint).native();
  static monitor::LatencyRecorder writeLatency("fuse.write.latency", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder writeDist("fuse.write.size", monitor::TagSet{{"mount_name", mountName}});

  auto uid = fuse_req_ctx(req)->uid;
  auto uids = std::to_string(uid);

  auto start = SteadyClock::now();

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).dryrun_bench_mode()) {
    fuse_reply_write(req, size);
    return;
  }

  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  writeDist.addSample(size, monitor::TagSet{{"uid", uids}});

  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL,
        "hf3fs_write(ino={}, buf={}, size={}, off={}, pid={})",
        ino,
        (uintptr_t)buf,
        size,
        off,
        fuse_req_ctx(req)->pid);
  record("write", fuse_req_ctx(req)->uid);

  auto odir = ((FileHandle *)fi->fh)->oDirect;
  auto pi = inodeOf(*fi, ino);
  //  auto &inode = pi->inode;

  std::unique_lock lock(pi->wbMtx);
  auto wb = pi->writeBuf;
  if (odir || !d.config->io_bufs().write_buf_size()) {
    if (!wb) {
      lock.unlock();
    } else {
      if (wb->len) {
        XLOGF(DBG, "to flush for o_direct fd  prev off {} len {}", off, wb->off, wb->len);
        if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
          wb->len = 0;
          return;
        }

        wb->len = 0;
      }
    }
    /*
        if (!d.buf) {
          XLOGF(INFO, "  hf3fs_write register buffer");
          d.buf.reset(std::make_unique<std::vector<uint8_t>>(d.maxBufsize));
          auto ret = d.storageClient->registerIOBuffer(d.buf->data(), d.maxBufsize);
          if (ret.hasError()) {
            handle_error(req, ret);
            return;
          } else {
            d.memh.reset(std::make_unique<IOBuffer>(std::move(*ret)));
          }
        }
    */
    auto memh = IOBuffer(folly::coro::blocking_wait(d.bufPool->allocate()));

    memcpy((char *)memh.data(), buf, size);
    auto ret = flushBuf(req, pi, off, memh, size, false);

    writeLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"uid", uids}});

    if (ret >= 0) {
      fuse_reply_write(req, ret);
    }
    return;
  }

  if (!wb) {
    auto wb2 = std::make_shared<InodeWriteBuf>();
    wb2->buf.resize(d.config->io_bufs().write_buf_size());
    auto ret = d.storageClient->registerIOBuffer(wb2->buf.data(), wb2->buf.size());
    if (!ret) {
      handle_error(req, ret);
      return;
    }
    wb2->memh.reset(new storage::client::IOBuffer(std::move(*ret)));
    pi->writeBuf = wb2;  //.compare_exchange_strong(wb, wb2);
    if (!wb) {
      wb = wb2;
    }
  }

  // std::lock_guard lock(wb->mtx);
  {
    if (wb->len && wb->off + (ssize_t)wb->len != off) {
      XLOGF(DBG, "to flush due to inconsecutive off {} prev off {} len {}", off, wb->off, wb->len);
      if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
        wb->len = 0;
        return;
      }

      wb->len = 0;
    }

    if (!wb->len) {
      wb->off = off;
    }

    size_t done = 0;
    do {
      auto cplen = std::min(size - done, wb->buf.size() - wb->len);
      memcpy(wb->buf.data() + wb->len, buf + done, cplen);
      wb->len += cplen;
      done += cplen;
      if (wb->len == wb->buf.size()) {
        XLOGF(DBG, "flush full buf {}", wb->len);
        if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
          wb->len = 0;
          return;
        }

        wb->len = 0;
        wb->off = off + done;
      }
    } while (done < size);

    if (wb->len) {
      // notify background task to flush buf and sync
      getFuseClientsInstance().dirtyInodes.lock()->insert(ino);
    }
  }

  writeLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"uid", uids}});
  fuse_reply_write(req, size);
}

void hf3fs_fsync(fuse_req_t req, fuse_ino_t fino, int datasync, struct fuse_file_info *fi) {
  XLOGF(DBG, "fsync called");
  XLOGF(OP_LOG_LEVEL, "hf3fs_sync(ino={}, datasync={}, pid={})", fino, datasync, fuse_req_ctx(req)->pid);
  if (datasync) {
    record("fdatasync", fuse_req_ctx(req)->uid);
  } else {
    record("fsync", fuse_req_ctx(req)->uid);
  }
  if (flushAndSync(req, fino, datasync && !d.config->fdatasync_update_length(), SyncType::Fsync, fi)) {
    fuse_reply_err(req, 0);
  }
}

void hf3fs_flush(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  hf3fs_fsync(req, fino, false, fi);

  // auto pi = inodeOf(*fi, ino);
  // auto wb = pi->writeBuf.load();
  // if (wb && wb->len) {
  //   if (flushBuf(req, pi, wb->off, *wb->memh, wb->len, true) < 0) {
  //     return;
  //   }
  // }

  // auto ino = real_ino(fino);

  // XLOGF(OP_LOG_LEVEL, "hf3fs_flush(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  // record("flush", fuse_req_ctx(req)->uid);

  // auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  // auto ptr = inodeOf(*fi, ino);
  // auto syncver = ptr->synced.load();
  // auto writever = ptr->written.load();
  // if (syncver < writever) {
  //   auto res = withRequestInfo(req, d.metaClient->sync(userInfo, ino, true, true));
  //   if (res.hasError()) {
  //     handle_error(req, res);
  //   } else {
  //     ptr->synced.store(writever);
  //     fuse_reply_err(req, 0);
  //   }
  // } else {
  //   fuse_reply_err(req, 0);
  // }
}

void hf3fs_release(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  auto ino = real_ino(fino);
  d.dirtyInodes.lock()->erase(ino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_release(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("release", fuse_req_ctx(req)->uid);

  SCOPE_EXIT { delete (FileHandle *)fi->fh; };

  auto sessionId = ((FileHandle *)fi->fh)->sessionId;
  auto ptr = inodeOf(*fi, ino);
  if (!ptr) {
    XLOGF(ERR, "hf3fs_release(ino={}): inode not found in list.", ino);
    fuse_reply_err(req, ENOENT);
    return;
  }

  //  ptr->opened--;

  {
    std::lock_guard lock(ptr->wbMtx);
    auto wb = ptr->writeBuf;
    if (wb && !wb->len) {
      ptr->writeBuf.reset();
    }
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if ((fi->flags & O_ACCMODE) == O_WRONLY || (fi->flags & O_ACCMODE) == O_RDWR) {
    auto res = withRequestInfo(req, close(userInfo, *ptr, sessionId));
    if (res.hasError()) {
      handle_error(req, res);
      return;
    }
  }

  fuse_reply_err(req, 0);
}

void hf3fs_opendir(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_opendir(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("opendir", fuse_req_ctx(req)->uid);

  fi->fh = (uintptr_t) new DirHandle{d.dirHandle.fetch_add(1), fuse_req_ctx(req)->pid, false};

  auto dname = checkVirtDir(ino);
  if (dname && *dname == "iovs") {
    ((DirHandle *)fi->fh)->iovDir = true;
  }

  fuse_reply_open(req, fi);
}

void hf3fs_releasedir(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fi) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_releasedir(ino={}, fh={}, pid={})", ino, fi->fh, fuse_req_ctx(req)->pid);
  record("releasedir", fuse_req_ctx(req)->uid);

  // {
  //   std::lock_guard lock{d.readdirResultsMutex};
  //   auto it = d.readdirResults.find(fi->fh);
  //   if (it != d.readdirResults.end()) {
  //     d.readdirResults.erase(it);
  //   }
  // }

  auto dh = (DirHandle *)fi->fh;

  {
    std::lock_guard lock{d.readdirplusResultsMutex};
    auto it = d.readdirplusResults.find(dh->dirId);
    if (it != d.readdirplusResults.end()) {
      d.readdirplusResults.erase(it);
    }
  }

  if (dh->iovDir) {
    // releasedir() is called only the last process with the inherited fd closes it or exits
    auto &iovs = *d.iovs.iovs;
    auto n = iovs.slots.nextAvail.load();
    for (int i = 0; i < n; ++i) {
      auto iov = iovs.table[i].load();
      if (iov && iov->pid == dh->pid) {
        XLOGF(INFO, "unlinking iov {} symlink from dead pid {}", iov->key, dh->pid);
        d.iovs.rmIov(iov->key.c_str(), meta::UserInfo{iov->user, meta::Gid{iov->user.toUnderType()}});
        if (iov->isIoRing) {
          d.iors.rmIoRing(iov->iorIndex);
        }
      }
    }
  }

  delete dh;

  fuse_reply_err(req, 0);
}

void hf3fs_statfs(fuse_req_t req, fuse_ino_t fino) {
  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_statfs(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("statfs", fuse_req_ctx(req)->uid);

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  auto res = withRequestInfo(req, d.metaClient->statFs(userInfo));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    struct ::statvfs buf;
    memset(&buf, 0, sizeof(buf));
    // to see if we enlarge the block size and reads/writes will be merged to bigger blocks
    buf.f_bsize = buf.f_frsize = 2 << 20;  // 512 << 10;
    buf.f_blocks = res.value().capacity / buf.f_bsize;
    buf.f_bfree = res.value().free / buf.f_bsize;
    buf.f_bavail = buf.f_bfree;  // TODO: quota
    buf.f_namemax = NAME_MAX;
    fuse_reply_statfs(req, &buf);
  }
}

void hf3fs_create(fuse_req_t req, fuse_ino_t fparent, const char *name, mode_t mode, struct fuse_file_info *fi) {
  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    fuse_reply_err(req, EROFS);
    return;
  }

  auto parent = real_ino(fparent);

  XLOGF(OP_LOG_LEVEL, "hf3fs_create(ino={}, name={}, mode={}, pid={})", parent, name, mode, fuse_req_ctx(req)->pid);
  record("create", fuse_req_ctx(req)->uid);

  if (checkVirtDir(parent)) {
    fuse_reply_err(req, EPERM);
    return;
  }

  if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode)) {
    fuse_reply_err(req, ENOSYS);
    return;
  }

  auto session = meta::client::SessionId::random();
  auto res = withRequestInfo(
      req,
      d.metaClient->create(userInfo, parent, name, session, meta::Permission(mode & ALLPERMS), fi->flags));
  if (res.hasError()) {
    handle_error(req, res);
  } else {
    struct fuse_entry_param e;
    init_entry(&e, d.userConfig.getConfig(userInfo).attr_timeout(), d.userConfig.getConfig(userInfo).entry_timeout());
    add_entry(res.value(), &e);

    auto ptr = inodeOf(*fi, res.value().id);

    fi->direct_io = (!d.userConfig.getConfig(userInfo).enable_read_cache() || fi->flags & O_DIRECT) ? 1 : 0;
    // fi->direct_io = 1;  // newly created file, has to write, or read from remote
    fi->fh = (uintptr_t)(new FileHandle{ptr, (bool)(fi->flags & O_DIRECT), session});
    XLOGF(DBG, "{}created in o direct mode", fi->flags & O_DIRECT ? "" : "not ");
    fuse_reply_create(req, &e, fi);
  }
}

std::string getCString(const char *cstr, size_t maxlen) {
  auto len = strnlen(cstr, maxlen);
  return std::string(cstr, len);
}

void hf3fs_ioctl(fuse_req_t req,
                 fuse_ino_t fino,
                 unsigned int cmd,
                 void *arg,
                 struct fuse_file_info *fi,
                 unsigned flags,
                 const void *in_buf,
                 size_t in_bufsz,
                 size_t out_bufsz) {
  (void)fi;
  (void)in_bufsz;

  auto ino = real_ino(fino);

  XLOGF(OP_LOG_LEVEL, "hf3fs_ioctl(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("ioctl", fuse_req_ctx(req)->uid);

  if (flags & FUSE_IOCTL_COMPAT) {
    fuse_reply_err(req, ENOSYS);
    return;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  switch (cmd) {
    case FS_IOC_GETFLAGS: {
      if (out_bufsz < sizeof(int)) {
        struct iovec iov = {arg, sizeof(int)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        auto res = withRequestInfo(req, d.metaClient->stat(userInfo, ino, {}, false));
        if (res.hasError()) {
          handle_error(req, res);
        } else {
          int iflags = res->acl.iflags;
          fuse_reply_ioctl(req, 0, &iflags, sizeof(iflags));
        }
      }
      break;
    }
    case FS_IOC_SETFLAGS: {
      if (in_bufsz < sizeof(int)) {
        fuse_reply_err(req, EINVAL);
      } else {
        int iflags = *(int *)in_buf;
        auto res = withRequestInfo(req, d.metaClient->setIFlags(userInfo, ino, meta::IFlags(iflags)));
        if (res.hasError()) {
          handle_error(req, res);
        } else {
          fuse_reply_ioctl(req, 0, nullptr, 0);
        }
      }
      break;
    }
    case FS_IOC_FSGETXATTR: {
      if (out_bufsz < sizeof(fsxattr)) {
        struct iovec iov = {arg, sizeof(fsxattr)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        auto res = withRequestInfo(req, d.metaClient->stat(userInfo, ino, {}, false));
        if (res.hasError()) {
          handle_error(req, res);
        } else {
          fsxattr xattr{};
          if (res->acl.iflags & FS_IMMUTABLE_FL) {
            xattr.fsx_xflags |= FS_XFLAG_IMMUTABLE;
          }
          fuse_reply_ioctl(req, 0, &xattr, sizeof(xattr));
        }
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_GET_MOUNT_NAME: {
      if (!out_bufsz) {
        struct iovec iov = {arg, sizeof(hf3fs::lib::fuse::Hf3fsIoctlGetMountNameArg)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        hf3fs::lib::fuse::Hf3fsIoctlGetMountNameArg ret;
        strcpy(ret.str, d.fuseMount.c_str());
        fuse_reply_ioctl(req, 0, &ret, sizeof(ret));
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_GET_PATH_OFFSET: {
      if (!out_bufsz) {
        struct iovec iov = {arg, sizeof(uint32_t)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        uint32_t offset = d.fuseMountpoint.size();
        fuse_reply_ioctl(req, 0, &offset, sizeof(offset));
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_GET_MAGIC_NUM: {
      if (!out_bufsz) {
        struct iovec iov = {arg, sizeof(uint32_t)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        uint32_t magic = HF3FS_SUPER_MAGIC;
        fuse_reply_ioctl(req, 0, &magic, sizeof(magic));
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_GET_IOCTL_VERSION: {
      // add a get version ioctl, application can check ioctl cmd is supported or not
      if (!out_bufsz) {
        struct iovec iov = {arg, sizeof(uint32_t)};
        fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      } else {
        uint32_t version = 1;
        fuse_reply_ioctl(req, 0, &version, sizeof(version));
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_RECURSIVE_RM: {
      // if (!out_bufsz) {
      //   struct iovec iov = {arg, sizeof(uint32_t)};
      //   fuse_reply_ioctl_retry(req, nullptr, 0, &iov, 1);
      // } else {
      auto res = withRequestInfo(req, [&userInfo, ino]() -> CoTryTask<void> {
        auto res = co_await d.metaClient->getRealPath(userInfo, ino, std::nullopt, true);
        if (res.hasError()) {
          co_return makeError(res.error());
        }
        co_return co_await d.metaClient->remove(userInfo, InodeId::root(), *res, true);
      }());
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        fuse_reply_ioctl(req, 0, nullptr, 0);
      }
      //      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_FSYNC: {
      auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
      auto pi = inodeOf(ino);
      auto res = flushAndSync(req, linux_ino(ino), false, SyncType::ForceFsync, nullptr);
      notify_inval_inode(ino);
      if (res) {
        fuse_reply_ioctl(req, 0, nullptr, 0);
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_HARDLINK: {
      auto newParent = InodeId(((hf3fs::lib::fuse::Hf3fsIoctlHardlinkArg *)in_buf)->ino);
      const char *name = ((hf3fs::lib::fuse::Hf3fsIoctlHardlinkArg *)in_buf)->str;
      auto res =
          withRequestInfo(req, d.metaClient->hardLink(userInfo, ino, std::nullopt, newParent, Path(name), false));
      if (res.hasError()) {
        handle_error(req, res);
      } else {
        fuse_reply_ioctl(req, 0, nullptr, 0);
      }
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_PUNCH_HOLE: {
      auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
      auto arg = (hf3fs::lib::fuse::Hf3fsIoctlPunchHoleArg *)in_buf;
      std::vector<storage::client::RemoveChunksOp> removeOps;
      auto pi = inodeOf(ino);
      auto file = pi->inode.asFile();
      auto chunkSize = file.layout.chunkSize;
      auto routingInfo = d.mgmtdClient->getRoutingInfo();
      for (int i = 0; i < arg->n; i++) {
        if (arg->start[i] % chunkSize != 0 || arg->end[i] % chunkSize != 0) {
          fuse_reply_err(req, EINVAL);
          return;
        }
        for (size_t off = arg->start[i]; off < arg->end[i]; off += chunkSize) {
          auto chainIdRes = file.getChainId(pi->inode, off, *routingInfo->raw());
          if (chainIdRes.hasError()) {
            handle_error(req, chainIdRes);
            return;
          }
          auto chainId = *chainIdRes;
          auto chunkId = file.getChunkId(ino, off);
          if (chunkId.hasError()) {
            handle_error(req, chunkId);
            return;
          }
          auto removeOp = d.storageClient->createRemoveOp(chainId,
                                                          storage::ChunkId(*chunkId),
                                                          storage::ChunkId(storage::ChunkId(*chunkId), 1));
          removeOps.push_back(std::move(removeOp));
        }
      }
      auto res = withRequestInfo(req, d.storageClient->removeChunks(removeOps, userInfo));
      if (res.hasError()) {
        handle_error(req, res);
        return;
      }
      for (auto &op : removeOps) {
        if (!op.status().isOK()) {
          fuse_reply_err(req, EIO);
          return;
        }
      }
      fuse_reply_ioctl(req, 0, nullptr, 0);
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_MOVE: {
      if (in_bufsz != sizeof(hf3fs::lib::fuse::Hf3fsIoctlMove)) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      auto move = (const hf3fs::lib::fuse::Hf3fsIoctlMove *)(in_buf);
      auto srcParent = real_ino(move->srcParent);
      auto srcName = Path(getCString(move->srcName, NAME_MAX));
      auto dstParent = real_ino(move->dstParent);
      auto dstName = Path(getCString(move->dstName, NAME_MAX));
      auto moveToTrash = move->moveToTrash;
      if (srcParent != ino) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      if (srcName.has_parent_path() || dstName.has_parent_path()) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      auto res =
          withRequestInfo(req, d.metaClient->rename(userInfo, srcParent, srcName, dstParent, dstName, moveToTrash));
      if (res.hasError()) {
        handle_error(req, res);
        return;
      }
      d.notifyInvalExec->add([srcParent, dstParent, srcName, dstName, req]() {
        notify_inval_entry(srcParent, srcName.string());
        notify_inval_entry(dstParent, dstName.string());
        fuse_reply_ioctl(req, 0, nullptr, 0);
      });
      break;
    }
    case hf3fs::lib::fuse::HF3FS_IOC_REMOVE: {
      if (in_bufsz != sizeof(hf3fs::lib::fuse::Hf3fsIoctlRemove)) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      auto remove = (const hf3fs::lib::fuse::Hf3fsIoctlRemove *)(in_buf);
      auto parent = real_ino(remove->parent);
      auto name = Path(getCString(remove->name, NAME_MAX));
      auto recursive = remove->recursive;
      if (parent != ino) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      if (name.has_parent_path()) {
        fuse_reply_err(req, EINVAL);
        return;
      }
      auto res = withRequestInfo(req, d.metaClient->remove(userInfo, parent, name, recursive));
      if (res.hasError()) {
        handle_error(req, res);
        return;
      }
      d.notifyInvalExec->add([parent, name, req]() {
        notify_inval_entry(parent, name.string());
        fuse_reply_ioctl(req, 0, nullptr, 0);
      });
      break;
    }
    default:
      fuse_reply_err(req, EINVAL);
  }
}

void hf3fs_readdirplus(fuse_req_t req, fuse_ino_t fino, size_t size, off_t off, struct fuse_file_info *fi) {
  static constexpr off_t kOffsetBegin = 2;
  if (off > kOffsetBegin) {
    off -= kOffsetBegin;
  }
  auto ino = real_ino(fino);

  auto did = ((DirHandle *)fi->fh)->dirId;

  XLOGF(OP_LOG_LEVEL,
        "hf3fs_readdirplus(ino={}, size={}, off={}, fh={}, pid={})",
        ino,
        size,
        off,
        did,
        fuse_req_ctx(req)->pid);
  record("readdirplus", fuse_req_ctx(req)->uid);

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);

  std::shared_ptr<std::vector<DirEntry>> pe;
  std::shared_ptr<std::vector<std::optional<Inode>>> pi;

  bool needsRpc = false;
  const Inode *pino;
  auto dname = checkVirtDir(ino, &pino);
  if (dname) {
    if (*dname == virtDir) {
      static folly::atomic_shared_ptr<std::vector<DirEntry>> rootEntries;
      static folly::atomic_shared_ptr<std::vector<std::optional<Inode>>> rootChildren;

      if (rootChildren.load()) {
        pe = rootEntries.load();
        pi = rootChildren.load();
      } else {
        pe = std::make_shared<std::vector<DirEntry>>();
        pi = std::make_shared<std::vector<std::optional<Inode>>>();
        pe->reserve(topVirtDirs.size());
        pi->reserve(topVirtDirs.size());
        for (const auto &ni : topVirtDirs) {
          pe->push_back(DirEntry(ino, ni.name, {ni.inode.id, ni.inode.data().getType(), ni.inode.data().acl}));
          pi->push_back(ni.inode);
        }

        rootEntries.store(pe);
        rootChildren.store(pi);
      }
    } else if (*dname == "iovs") {
      std::tie(pe, pi) = d.iovs.listIovs(userInfo);
      std::lock_guard lock{d.readdirplusResultsMutex};
      d.readdirplusResults.insert({did, DirEntryInodeVector{pe, pi}});
    } else if (*dname == "get-conf") {
      std::tie(pe, pi) = d.userConfig.listConfig(userInfo);
      std::lock_guard lock{d.readdirplusResultsMutex};
      d.readdirplusResults.insert({did, DirEntryInodeVector{pe, pi}});
    } else {
      pe = std::make_shared<std::vector<DirEntry>>();
    }
  }

  if (!pe) {
    std::lock_guard lock{d.readdirplusResultsMutex};
    auto it = d.readdirplusResults.find(did);
    if (it == d.readdirplusResults.end()) {
      needsRpc = true;
    }
  }

  if (needsRpc) {
    std::vector<DirEntry> entries;
    std::vector<std::optional<Inode>> inodes;

    bool hasNext = true;
    std::string_view prev;
    while (hasNext) {
      auto ret = withRequestInfo(req, d.metaClient->list(userInfo, ino, std::nullopt, prev, 256, false));
      if (ret.hasError()) {
        handle_error(req, ret);
        return;
      } else {
        for (auto &ent : ret->entries) {
          entries.push_back(std::move(ent));
          inodes.push_back(std::nullopt);
        }
        hasNext = ret->more;
        if (hasNext) {
          prev = entries.back().name;
        }
      }
    }

    if (ino == InodeId::root()) {
      entries.push_back(
          DirEntry(ino, virtDir, {virtDirInode.id, virtDirInode.data().getType(), virtDirInode.data().acl}));
      inodes.push_back(virtDirInode);
    }

    std::lock_guard lock{d.readdirplusResultsMutex};
    d.readdirplusResults.insert(
        {did,
         DirEntryInodeVector{std::make_shared<std::vector<DirEntry>>(std::move(entries)),
                             std::make_shared<std::vector<std::optional<Inode>>>(std::move(inodes))}});
  }

  auto rem = size;
  auto buf = std::make_unique<char[]>(size);
  auto p = buf.get();

  if (!pe) {
    std::lock_guard lock{d.readdirplusResultsMutex};
    pe = d.readdirplusResults.find(did)->second.dirEntries;
    pi = d.readdirplusResults.find(did)->second.inodes;
  }

  const auto &entries = *pe;
  const auto &inodes = *pi;
  off_t last = off;
  std::vector<std::optional<Inode>> realInodes;
  int64_t cursize = 0;
  while ((size_t)last < entries.size()) {
#define FUSE_NAME_OFFSET_DIRENTPLUS 152
#define FUSE_REC_ALIGN(x) (((x) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1))
#define FUSE_DIRENT_ALIGN(x) FUSE_REC_ALIGN(x)

    auto entsize = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET_DIRENTPLUS + entries[last].name.size());
    if (cursize + entsize < size) {
      cursize += entsize;
      last++;
    } else {
      break;
    }
  }

  std::vector<InodeId> queryIds;
  for (auto i = off; i < last; i++) {
    if (!inodes[i].has_value()) {
      queryIds.push_back(entries[i].id);
    }
  }
  auto queryRet = withRequestInfo(req, d.metaClient->batchStat(userInfo, queryIds));
  if (queryRet.hasError()) {
    handle_error(req, queryRet);
    return;
  }
  for (auto i = off, cur = 0L; i < last; i++) {
    if (!inodes[i].has_value()) {
      realInodes.push_back(queryRet.value()[cur++]);
    } else {
      realInodes.push_back(inodes[i]);
    }
  }

  for (auto i = off; i < last; i++) {
    auto idx = i - off;
    std::string name;
    struct fuse_entry_param e;

    const auto &ent = entries[i];
    name = ent.name;

    if (realInodes[idx].has_value()) {
      if (realInodes[idx]->isSymlink()) {
        init_entry(&e,
                   d.userConfig.getConfig(userInfo).symlink_timeout(),
                   d.userConfig.getConfig(userInfo).symlink_timeout());
      } else {
        init_entry(&e,
                   d.userConfig.getConfig(userInfo).attr_timeout(),
                   d.userConfig.getConfig(userInfo).entry_timeout());
      }
      XLOGF(DBG,
            "inode id {} uid {} permission {}",
            realInodes[idx]->id,
            realInodes[idx]->data().acl.uid,
            realInodes[idx]->data().acl.perm);
      add_entry(realInodes[idx].value(), &e);
      XLOGF(DBG, "entry id {} uid {} permission {}", e.attr.st_ino, e.attr.st_uid, e.attr.st_mode);

      //    std::cout << "adding dentry " << name << " i " << i << " size " << entries.size() << std::endl;
      auto entsize = fuse_add_direntry_plus(req, p, rem, name.data(), &e, i + 1 + kOffsetBegin);
      if (entsize > rem) {
        remove_entry(realInodes[idx]->id, 1);
        break;
      }
      p += entsize;
      rem -= entsize;
    } else {
      add_empty_entry(entries[i].id);
      struct fuse_entry_param e;
      memset(&e, 0, sizeof(e));
      auto entsize = fuse_add_direntry_plus(req, p, rem, name.data(), &e, i + 1 + kOffsetBegin);
      if (entsize > rem) {
        remove_entry(entries[i].id, 1);
        break;
      }
      p += entsize;
      rem -= entsize;
    }
  }

  fuse_reply_buf(req, buf.get(), size - rem);
}

void hf3fs_setxattr(fuse_req_t req,
                    fuse_ino_t fino,
                    const char *cname,
                    const char *cvalue,
                    size_t size,
                    int /* flags */) {
  auto ino = real_ino(fino);
  auto name = folly::StringPiece(cname);
  auto value = folly::StringPiece(cvalue, size);

  XLOGF(OP_LOG_LEVEL, "hf3fs_setxattr(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("setxattr", fuse_req_ctx(req)->uid);

  // only support hf3fs.xxx
  if (!name.startsWith("hf3fs.")) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }

  // only support on directory
  const Inode *pino;
  if (checkVirtDir(ino, &pino)) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }
  auto pi = inodeOf(ino);
  if (!pi) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }
  if (!pi->inode.isDirectory()) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (name == "hf3fs.lock") {
    static std::map<std::string, meta::LockDirectoryReq::LockAction, std::less<>> map{
        {"try_lock", meta::LockDirectoryReq::LockAction::TryLock},
        {"preempt_lock", meta::LockDirectoryReq::LockAction::PreemptLock},
        {"unlock", meta::LockDirectoryReq::LockAction::UnLock},
        {"clear", meta::LockDirectoryReq::LockAction::Clear}};
    auto vstr = value.str();
    if (map.contains(std::string(vstr))) {
      auto result = withRequestInfo(req, d.metaClient->lockDirectory(userInfo, ino, map[vstr]));
      if (!result) {
        handle_error(req, result);
        return;
      } else {
        fuse_reply_err(req, 0);
        return;
      }
    } else {
      XLOGF(ERR, "invalid value {} for hf3fs.lock, support: try_lock, preempt_lock, unlock, clear!", value);
      fuse_reply_err(req, EINVAL);
      return;
    }
  } else {
    XLOGF(ERR, "try to set invalid key {}", name);
    fuse_reply_err(req, ENOTSUP);
    return;
  }
}

void hf3fs_getxattr(fuse_req_t req, fuse_ino_t fino, const char *cname, size_t size) {
  auto ino = real_ino(fino);
  auto name = folly::StringPiece(cname);

  XLOGF(OP_LOG_LEVEL, "hf3fs_getxattr(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("getxattr", fuse_req_ctx(req)->uid);

  // don't support on virtual directory
  const Inode *pino;
  if (checkVirtDir(ino, &pino)) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }
  auto pi = inodeOf(ino);
  if (!pi || pi->inode.isSymlink()) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  auto value = std::optional<std::string>();
  if (name == "hf3fs.lock") {
    if (!pi->inode.isDirectory()) {
      fuse_reply_err(req, ENODATA);
      return;
    }
    auto stat = withRequestInfo(req, d.metaClient->stat(userInfo, ino, std::nullopt, false));
    if (!stat) {
      handle_error(req, stat);
      return;
    }
    if (!stat->isDirectory() || !stat->asDirectory().lock) {
      fuse_reply_err(req, ENODATA);
      return;
    }
    value = serde::toJsonString(*stat->asDirectory().lock);
  }

  if (!value) {
    fuse_reply_err(req, ENODATA);
    return;
  }

  if (!size) {
    fuse_reply_xattr(req, value->size());
  } else if (size < value->size()) {
    fuse_reply_err(req, ERANGE);
  } else {
    fuse_reply_buf(req, value->c_str(), value->size());
  }
}

void hf3fs_listxattr(fuse_req_t req, fuse_ino_t fino, size_t size) {
  auto ino = real_ino(fino);
  XLOGF(OP_LOG_LEVEL, "hf3fs_listxattr(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("listxattr", fuse_req_ctx(req)->uid);

  // don't support on virtual directory
  const Inode *pino;
  if (checkVirtDir(ino, &pino)) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }
  auto pi = inodeOf(ino);
  if (!pi || pi->inode.isSymlink()) {
    fuse_reply_err(req, ENOTSUP);
    return;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  std::set<std::string> set;
  if (pi->inode.isDirectory()) {
    auto stat = withRequestInfo(req, d.metaClient->stat(userInfo, ino, std::nullopt, false));
    if (!stat) {
      handle_error(req, stat);
      return;
    } else if (stat->asDirectory().lock) {
      set.insert("hf3fs.lock");
    }
  }

  auto bytes = std::accumulate(set.begin(), set.end(), 0ull, [](auto val, auto &key) { return val + key.size() + 1; });
  if (!size) {
    fuse_reply_xattr(req, bytes);
    return;
  } else if (size < bytes) {
    fuse_reply_err(req, ERANGE);
    return;
  }

  std::vector<char> buffer;
  buffer.reserve(bytes);
  for (const auto &key : set) {
    buffer.insert(buffer.end(), key.begin(), key.end());
    buffer.push_back('\0');
  }
  XLOGF_IF(FATAL, buffer.size() != bytes, "{} != {}", buffer.size(), bytes);
  fuse_reply_buf(req, buffer.data(), buffer.size());
}

void hf3fs_removexattr(fuse_req_t req, fuse_ino_t fino, const char *cname) {
  auto ino = real_ino(fino);
  auto name = folly::StringPiece(cname);
  XLOGF(OP_LOG_LEVEL, "hf3fs_removexattr(ino={}, pid={})", ino, fuse_req_ctx(req)->pid);
  record("removexattr", fuse_req_ctx(req)->uid);

  // don't support on virtual directory
  const Inode *pino;
  if (checkVirtDir(ino, &pino)) {
    fuse_reply_err(req, EPERM);
    return;
  }
  auto pi = inodeOf(ino);
  if (!pi || pi->inode.isSymlink()) {
    fuse_reply_err(req, EPERM);
    return;
  }

  auto userInfo = UserInfo(flat::Uid(fuse_req_ctx(req)->uid), flat::Gid(fuse_req_ctx(req)->gid), d.fuseToken);
  if (name == "hf3fs.lock") {
    if (!pi->inode.isDirectory()) {
      fuse_reply_err(req, ENOTDIR);
      return;
    }
    auto result =
        withRequestInfo(req, d.metaClient->lockDirectory(userInfo, ino, meta::LockDirectoryReq::LockAction::Clear));
    if (!result) {
      handle_error(req, result);
      return;
    } else {
      fuse_reply_err(req, 0);
      return;
    }
  } else {
    fuse_reply_err(req, EPERM);
    return;
  }
}
}  // namespace

const fuse_lowlevel_ops hf3fs_oper = {
    .init = hf3fs_init,
    .destroy = hf3fs_destroy,
    .lookup = hf3fs_lookup,
    .forget = hf3fs_forget,
    .getattr = hf3fs_getattr,
    .setattr = hf3fs_setattr,
    .readlink = hf3fs_readlink,
    .mknod = hf3fs_mknod,
    .mkdir = hf3fs_mkdir,
    .unlink = hf3fs_unlink,
    .rmdir = hf3fs_rmdir,
    .symlink = hf3fs_symlink,
    .rename = hf3fs_rename,
    .link = hf3fs_link,
    .open = hf3fs_open,
    .read = hf3fs_read,
    .write = hf3fs_write,
    .flush = hf3fs_flush,
    .release = hf3fs_release,
    .fsync = hf3fs_fsync,
    .opendir = hf3fs_opendir,
    //    .readdir = hf3fs_readdir,
    .releasedir = hf3fs_releasedir,
    //.fsyncdir = hf3fs_fsyncdir,
    .statfs = hf3fs_statfs,
    .setxattr = hf3fs_setxattr,
    .getxattr = hf3fs_getxattr,
    .listxattr = hf3fs_listxattr,
    .removexattr = hf3fs_removexattr,
    .create = hf3fs_create,
    .ioctl = hf3fs_ioctl,
    .readdirplus = hf3fs_readdirplus,
};

const fuse_lowlevel_ops &getFuseOps() { return hf3fs_oper; }

CoTryTask<uint64_t> RcInode::beginWrite(flat::UserInfo userInfo,
                                        meta::client::MetaClient &meta,
                                        uint64_t offset,
                                        uint64_t length) {
  auto stripe = std::min((uint32_t)folly::divCeil(offset + length, (uint64_t)inode.asFile().layout.chunkSize),
                         inode.asFile().layout.stripeSize);
  {
    auto guard = dynamicAttr.rlock();
    if (!guard->dynStripe || guard->dynStripe >= stripe) {
      // don't need update dynamic stripe
      co_return guard->truncateVer;
    }
  }

  // only allow 1 task extend stripe
  co_await extendStripeLock.co_lock();
  SCOPE_EXIT { extendStripeLock.unlock(); };
  {
    // check stripe again
    auto guard = dynamicAttr.rlock();
    if (!guard->dynStripe || guard->dynStripe >= stripe) {
      // don't need update dynamic stripe
      co_return guard->truncateVer;
    }
  }

  auto res = co_await meta.extendStripe(userInfo, inode.id, stripe);
  CO_RETURN_ON_ERROR(res);

  auto guard = dynamicAttr.wlock();
  guard->update(*res);
  if (guard->dynStripe && guard->dynStripe < stripe) {
    XLOGF(DFATAL, "after extend stripe, {} < {}", guard->dynStripe, stripe);
    co_return makeError(MetaCode::kFoundBug, fmt::format("{} < {} after extend", guard->dynStripe, stripe));
  }
  co_return guard->truncateVer;
}

void RcInode::finishWrite(flat::UserInfo userInfo, uint64_t truncateVer, uint64_t offset, ssize_t ret) {
  std::optional<meta::VersionedLength> newHint = std::nullopt;
  if (ret >= 0) {
    newHint = meta::VersionedLength{ret ? offset + ret : 0, truncateVer};
  }

  auto guard = dynamicAttr.wlock();
  if (userInfo.uid) {
    guard->writer = flat::Uid(userInfo.uid);
  }
  guard->written++;
  guard->hintLength = meta::VersionedLength::mergeHint(guard->hintLength, newHint);
  guard->mtime = UtcClock::now();
  getFuseClientsInstance().dirtyInodes.lock()->insert(inode.id);
}

CoTask<void> FuseClients::periodicSync(InodeId inodeId) {
  if (!config->periodic_sync().enable()) {
    co_return;
  }

  auto pi = inodeOf(inodeId);
  if (!pi) {
    XLOGF(INFO, "Inode {} not found", inodeId);
    co_return;
  }

  XLOGF_IF(FATAL, inodeId != pi->inode.id, "want {}, found {}", inodeId, pi->inode);
  XLOGF(DBG, "PeriodSync {}", pi->inode.id);

  auto writer = pi->dynamicAttr.rlock()->writer;
  auto userInfo = UserInfo(writer, flat::Gid(writer), fuseToken);
  if (d.userConfig.getConfig(userInfo).readonly()) {
    co_return;
  }

  // try flush buf first
  if (config->periodic_sync().flush_write_buf() && pi->wbMtx.try_lock()) {
    SCOPE_EXIT { pi->wbMtx.unlock(); };

    auto wb = pi->writeBuf;
    if (wb && wb->len) {
      auto result = co_await flushBuf(userInfo, pi, wb->off, *wb->memh, wb->len, true);
      if (result.hasError()) {
        XLOGF(WARN, "{} flush buf failed", inodeId);
        co_return;
      }
      wb->len = 0;
    }
  }

  auto res = co_await sync(userInfo, *pi, SyncType::PeriodSync);
  if (res.hasError()) {
    XLOGF(ERR, "period sync {} failed, error {}", inodeId, res.error());
  } else if (*res) {
    record("periodic_sync", writer);
  }

  co_return;
}

}  // namespace hf3fs::fuse
