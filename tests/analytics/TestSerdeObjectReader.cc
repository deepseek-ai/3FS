#include <arrow/io/file.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <parquet/schema.h>

#include "analytics/SerdeObjectReader.h"
#include "analytics/SerdeObjectWriter.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {

template <serde::SerdeType T>
void readAndCompareSerdeObjectDump(const T &expected) {
  {
    auto writer = SerdeObjectWriter<T>::open(fmt::format("{}.parquet", nameof::nameof_short_type<T>()));
    ASSERT_NE(writer, nullptr);
    *writer << expected << parquet::EndRowGroup;
  }

  auto reader = SerdeObjectReader<T>::open(fmt::format("{}.parquet", nameof::nameof_short_type<T>()));
  ASSERT_NE(reader, nullptr);
  T fromFile;
  *reader >> fromFile;

  ASSERT_TRUE(serde::equals(expected, fromFile))
      << "Expected: " << serde::toJsonString(expected, true, true) << std::endl
      << "FromFile: " << serde::toJsonString(fromFile, true, true);
}

TEST(TestSerdeObjectReader, ChunkMeta) {
  storage::ChunkMeta chunkmeta{
      .chunkId = storage::ChunkId(1, 1),
      .updateVer = storage::ChunkVer(1),
      .commitVer = storage::ChunkVer(2),
      .checksum =
          storage::ChecksumInfo{
              .type = storage::ChecksumType::CRC32C,
              .value = 123,
          },
  };

  readAndCompareSerdeObjectDump(chunkmeta);
}

TEST(TestSerdeObjectReader, Inode) {
  meta::Inode inode{
      meta::InodeId{0xFF},
      meta::InodeData{
          .type =
              meta::File{
                  meta::Layout{
                      .tableId = meta::ChainTableId{1},
                      .tableVersion = meta::ChainTableVersion{1},
                      .chains = meta::Layout::ChainRange(101 /*baseIndex*/,
                                                         meta::Layout::ChainRange::Shuffle::STD_SHUFFLE_MT19937,
                                                         0xFFEE /*seed*/),
                  },
              },
          .nlink = 10,
          .atime = UtcTime::clock::now(),
          .ctime = UtcTime::clock::now(),
          .mtime = UtcTime::clock::now(),
      },
  };

  readAndCompareSerdeObjectDump(inode);
}

TEST(TestSerdeObjectReader, DirEntry) {
  meta::DirEntry entry{meta::InodeId{0x10},
                       "direntry",
                       meta::DirEntryData{meta::InodeId{0xFF},
                                          meta::InodeType::File,
                                          meta::Acl(flat::Uid(1), flat::Gid(1), flat::Permission(0644))}};

  readAndCompareSerdeObjectDump(entry);
  entry.dirAcl = std::nullopt;
  readAndCompareSerdeObjectDump(entry);
}

TEST(TestSerdeObjectReader, IOResult) {
  storage::IOResult ioResult{
      1024U,
      storage::ChunkVer{1},
      storage::ChunkVer{2},
      storage::ChecksumInfo{storage::ChecksumType::CRC32C, 0xFFEEAABB},
      storage::ChainVer{123},
  };

  readAndCompareSerdeObjectDump(ioResult);
}

TEST(TestSerdeObjectReader, UpdateReq) {
  storage::UpdateReq updateReq{
      .payload =
          {
              .offset = 123,
              .length = 456,
              .chunkSize = 789,
              .key =
                  storage::GlobalKey{
                      .vChainId =
                          storage::VersionedChainId{
                              .chainId = storage::ChainId{10001},
                              .chainVer = storage::ChainVer{2000},
                          },
                  },
              // .rdmabuf = net::RDMARemoteBuf{},
              .updateVer = storage::ChunkVer{999},
              .updateType = storage::UpdateType::WRITE,
              .checksum =
                  storage::ChecksumInfo{
                      .type = storage::ChecksumType::CRC32C,
                      .value = 0xFFEEAABB,
                  },
              // .inlinebuf =
              //     storage::UInt8Vector{
              //         .data = std::vector<uint8_t>(10, 0xFF),
              //     },
          },
      .tag =
          storage::MessageTag{
              ClientId::random(),
              storage::RequestId{101},
              storage::UpdateChannel{
                  .id = storage::ChannelId{1},
                  .seqnum = storage::ChannelSeqNum{1},
              },
          },
  };

  readAndCompareSerdeObjectDump(updateReq);
}

}  // namespace hf3fs::analytics
