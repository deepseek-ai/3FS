#pragma once

#include <folly/Range.h>

#include "common/utils/Result.h"
#include "storage/aio/BatchReadJob.h"
#include "storage/store/ChunkMetadata.h"
#include "storage/store/ChunkStore.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {

class ChunkReplica {
 public:
  // prepare aio read.
  static Result<Void> aioPrepareRead(ChunkStore &store, AioReadJob &job);

  // finish aio read.
  static Result<Void> aioFinishRead(ChunkStore &store, AioReadJob &job);

  // do write.
  static Result<uint32_t> update(ChunkStore &store, UpdateJob &job, folly::CPUThreadPoolExecutor &executor);

  static Result<Void> updateChecksum(ChunkInfo &chunkInfo,
                                     UpdateIO writeIO,
                                     uint32_t chunkSizeBeforeWrite,
                                     bool isAppendWrite);

  // commit the version of this chunk.
  static Result<uint32_t> commit(ChunkStore &store, UpdateJob &job);
};

}  // namespace hf3fs::storage
