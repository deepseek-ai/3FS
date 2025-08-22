#pragma once

#include <sys/types.h>
#include <sys/vfs.h>

#include "common/serde/Serde.h"
#include "common/utils/Uuid.h"

namespace hf3fs::lib::agent {
struct ProcessInfo {
  SERDE_STRUCT_TYPED_FIELD(int, pid, 0);
  SERDE_STRUCT_TYPED_FIELD(int, ppid, 0);
};

struct TimeSpec {
  SERDE_STRUCT_TYPED_FIELD(time_t, sec, 0);
  SERDE_STRUCT_TYPED_FIELD(long, nsec, 0);
};

struct Stat {
  SERDE_STRUCT_TYPED_FIELD(dev_t, dev, 0);
  SERDE_STRUCT_TYPED_FIELD(ino_t, ino, 0);
  SERDE_STRUCT_TYPED_FIELD(mode_t, mode, 0);
  SERDE_STRUCT_TYPED_FIELD(nlink_t, nlink, 0);
  SERDE_STRUCT_TYPED_FIELD(uid_t, uid, 0);
  SERDE_STRUCT_TYPED_FIELD(gid_t, gid, 0);
  SERDE_STRUCT_TYPED_FIELD(dev_t, rdev, 0);
  SERDE_STRUCT_TYPED_FIELD(off_t, size, 0);
  SERDE_STRUCT_TYPED_FIELD(blksize_t, blksize, 0);
  SERDE_STRUCT_TYPED_FIELD(blkcnt_t, blocks, 0);
  SERDE_STRUCT_TYPED_FIELD(TimeSpec, atim, TimeSpec{});
  SERDE_STRUCT_TYPED_FIELD(TimeSpec, mtim, TimeSpec{});
  SERDE_STRUCT_TYPED_FIELD(TimeSpec, ctim, TimeSpec{});
  SERDE_STRUCT_TYPED_FIELD(std::optional<String>, ltarg, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(Path, path, Path{"/"});
};

struct StatFs {
  SERDE_STRUCT_TYPED_FIELD(__fsword_t, type, 0);
  SERDE_STRUCT_TYPED_FIELD(__fsword_t, bsize, 0);
  SERDE_STRUCT_TYPED_FIELD(fsblkcnt_t, blocks, 0);
  SERDE_STRUCT_TYPED_FIELD(fsblkcnt_t, bfree, 0);
  SERDE_STRUCT_TYPED_FIELD(fsblkcnt_t, bavail, 0);
  SERDE_STRUCT_TYPED_FIELD(fsfilcnt_t, files, 0);
  SERDE_STRUCT_TYPED_FIELD(fsfilcnt_t, ffree, 0);
  SERDE_STRUCT_TYPED_FIELD(long, fsid, 0);
  SERDE_STRUCT_TYPED_FIELD(__fsword_t, namelen, 0);
  SERDE_STRUCT_TYPED_FIELD(__fsword_t, frsize, 0);
  SERDE_STRUCT_TYPED_FIELD(__fsword_t, flags, 0);
};

struct DirEnt {
  SERDE_STRUCT_TYPED_FIELD(unsigned char, type, 0);
  SERDE_STRUCT_TYPED_FIELD(String, name, "");
};

struct DirEntList {
  SERDE_STRUCT_TYPED_FIELD(std::vector<DirEnt>, dents, std::vector<DirEnt>{});
  SERDE_STRUCT_TYPED_FIELD(bool, hasMore, false);
};

struct SharedFileHandles {
  SERDE_STRUCT_TYPED_FIELD(std::vector<String>, fhs, std::vector<String>{});
};

struct AllocatedIov {
  SERDE_STRUCT_TYPED_FIELD(int, iovDesc, 0);
  SERDE_STRUCT_TYPED_FIELD(Path, path, Path{});
  SERDE_STRUCT_TYPED_FIELD(long, offInBuf, 0);
  SERDE_STRUCT_TYPED_FIELD(ulong, bytes, 0);
  SERDE_STRUCT_TYPED_FIELD(Uuid, id, Uuid{});
  SERDE_STRUCT_TYPED_FIELD(ulong, blockSize, 0);
};

struct IoInfo {
  SERDE_STRUCT_TYPED_FIELD(int, iovDesc, 0);
  SERDE_STRUCT_TYPED_FIELD(Uuid, iovId, Uuid{});
  SERDE_STRUCT_TYPED_FIELD(long, iovOff, 0);
  SERDE_STRUCT_TYPED_FIELD(ulong, iovLen, 0);

  SERDE_STRUCT_TYPED_FIELD(int, fd, 0);
  SERDE_STRUCT_TYPED_FIELD(long, off, 0);
  SERDE_STRUCT_TYPED_FIELD(ulong, readahead, 0);

  SERDE_STRUCT_TYPED_FIELD(uint16_t, track, 0);
};

struct PioVRes {
  SERDE_STRUCT_TYPED_FIELD(std::vector<long>, res, std::vector<long>{});
  SERDE_STRUCT_TYPED_FIELD(std::vector<long>, offs, std::vector<long>{});
};
}  // namespace hf3fs::lib::agent
