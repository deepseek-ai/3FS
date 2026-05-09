#pragma once

#include <cstdint>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/serde/Serde.h"
#include "fbs/meta/Schema.h"
namespace hf3fs::client::cli {

struct DirEntryRow {
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(entry, meta::DirEntry{});
};

struct DirEntryTable {
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(entries, std::vector<DirEntryRow>());

 public:
  bool dumpToParquetFile(const Path &filePath) const;
};
static_assert(serde::Serializable<DirEntryTable>);

CoTryTask<Void> dumpDirEntriesFromFdb(std::shared_ptr<kv::IKVEngine> kvEngine,
                                      const uint32_t numEntriesPerDir,
                                      const std::string dentryDir,
                                      const uint32_t threads);
class Dispatcher;
CoTryTask<void> registerDumpDirEntriesHandler(Dispatcher &dispatcher);
}  // namespace hf3fs::client::cli