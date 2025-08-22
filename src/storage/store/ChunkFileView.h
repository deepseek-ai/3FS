#pragma once

#include <folly/Range.h>

#include "common/utils/Result.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage {

class ChunkFileView {
 public:
  // read a piece of data.
  Result<uint32_t> read(uint8_t *buf, size_t size, size_t offset, bool direct = false) const;

  // write a piece of data.
  Result<uint32_t> write(const uint8_t *buf, size_t size, size_t offset, const ChunkMetadata &meta);

  // calculate the chunk checksum
  Result<ChecksumInfo> checksum(ChecksumType type, size_t size, size_t offset, const ChunkMetadata &meta);

  // get direct fd for aio read.
  int directFD() const { return direct_; }

  // get fd index in list.
  auto &index() const { return index_; }

 private:
  friend class ChunkFileStore;
  int normal_;
  int direct_;
  std::optional<uint32_t> index_{};
};

class ChunkDataIterator : public ChecksumInfo::DataIterator {
 public:
  ChunkDataIterator(ChunkFileView &chunkFile, size_t length, size_t offset)
      : data_((uint8_t *)memory::memalign(kAIOAlignSize, ChecksumInfo::kChunkSize)),
        chunkFile_(chunkFile),
        length_(length),
        offset_(offset) {}

  ~ChunkDataIterator() override { memory::deallocate(data_); }

  std::pair<const uint8_t *, size_t> next() override;

 private:
  uint8_t *data_;
  ChunkFileView &chunkFile_;
  size_t length_;
  size_t offset_;
};

}  // namespace hf3fs::storage
