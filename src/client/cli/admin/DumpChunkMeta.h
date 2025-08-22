#pragma once

#include <parquet/schema.h>

#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {

class ChunkMetaRow {
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(chainId, storage::ChainId{});
  SERDE_STRUCT_FIELD(targetId, storage::TargetId{});
  SERDE_STRUCT_FIELD(inodeId, meta::InodeId{});
  SERDE_STRUCT_FIELD(chunkmeta, storage::ChunkMeta{});
};

struct ChunkMetaTable {
  SERDE_STRUCT_FIELD(chainId, storage::ChainId{});
  SERDE_STRUCT_FIELD(targetId, storage::TargetId{});
  SERDE_STRUCT_FIELD(timestamp, std::time_t{});
  SERDE_STRUCT_FIELD(chunks, std::vector<ChunkMetaRow>{});

 public:
  bool dumpToFile(const Path &filePath, bool jsonFormat = false) const;
  bool loadFromFile(const Path &filePath, bool jsonFormat = false);
  bool dumpToParquetFile(const Path &filePath) const;
  bool loadFromParquetFile(const Path &filePath);
};
static_assert(serde::Serializable<ChunkMetaTable>);

class Dispatcher;
CoTryTask<void> registerDumpChunkMetaHandler(Dispatcher &dispatcher);
}  // namespace hf3fs::client::cli