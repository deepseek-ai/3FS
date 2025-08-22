#include <arrow/io/file.h>
#include <folly/logging/xlog.h>
#include <parquet/schema.h>

#include "analytics/SerdeObjectWriter.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {

TEST(TestSerdeObjectWriter, ChunkMeta) {
  auto writer = SerdeObjectWriter<storage::ChunkMeta>::open("TestSerdeObjectWriter.ChunkMeta.parquet");
  ASSERT_NE(writer, nullptr);
  storage::ChunkMeta chunkmeta;
  *writer << chunkmeta << parquet::EndRowGroup;
}

TEST(TestSerdeObjectWriter, Inode) {
  auto writer = SerdeObjectWriter<meta::Inode>::open("TestSerdeObjectWriter.Inode.parquet");
  ASSERT_NE(writer, nullptr);
  meta::Inode inode;
  *writer << inode << parquet::EndRowGroup;
}

TEST(TestSerdeObjectWriter, DirEntry) {
  auto writer = SerdeObjectWriter<meta::DirEntry>::open("TestSerdeObjectWriter.DirEntry.parquet");
  ASSERT_NE(writer, nullptr);
  meta::DirEntry entry;
  *writer << entry << parquet::EndRowGroup;
}

TEST(TestSerdeObjectWriter, UpdateReq) {
  auto writer = SerdeObjectWriter<storage::UpdateReq>::open("TestSerdeObjectWriter.UpdateReq.parquet");
  ASSERT_NE(writer, nullptr);
  storage::UpdateReq updateReq;
  *writer << updateReq << parquet::EndRowGroup;
}

}  // namespace hf3fs::analytics
