#pragma once

#include "common/kv/IKVEngine.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::client::cli {

struct InodeRow {
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(inode, meta::Inode{});
};

struct InodeTable {
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(inodes, std::vector<InodeRow>{});

 public:
  bool dumpToFile(const Path &filePath) const;
  bool loadFromFile(const Path &filePath);
  bool dumpToParquetFile(const Path &filePath) const;
  bool loadFromParquetFile(const Path &filePath);
};
static_assert(serde::Serializable<InodeTable>);

std::vector<Path> listFilesFromPath(const Path path);

CoTryTask<Void> dumpInodesFromFdb(std::shared_ptr<kv::IKVEngine> kvEngine,
                                  const uint32_t numInodesPerFile,
                                  const std::string inodeDir,
                                  const bool parquetFormat = false,
                                  const bool dumpAllInodes = false,
                                  const uint32_t threads = 4);

CoTryTask<robin_hood::unordered_set<meta::InodeId>> loadInodeFromFiles(
    const Path inodePath,
    const robin_hood::unordered_set<meta::InodeId> &inodeIdsToPeek,
    const uint32_t parallel,
    const bool parquetFormat = false,
    std::time_t *inodeDumpTime = nullptr);

class Dispatcher;
CoTryTask<void> registerDumpInodesHandler(Dispatcher &dispatcher);
}  // namespace hf3fs::client::cli