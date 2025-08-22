#include <folly/logging/xlog.h>
#include <parquet/schema.h>

#include "analytics/SerdeSchemaBuilder.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {
using namespace parquet;

TEST(TestSerdeSchemaBuilder, ChunkMeta) {
  SerdeSchemaBuilder<storage::ChunkMeta> builder;
  auto chunkmetaSchema = builder.getSchema();

  std::ostringstream outstr;
  schema::PrintSchema(chunkmetaSchema.get(), outstr);
  XLOGF(INFO, "chunkmeta schema: {}", outstr.str());
}

TEST(TestSerdeSchemaBuilder, Inode) {
  SerdeSchemaBuilder<meta::Inode> builder;
  auto inodeSchema = builder.getSchema();

  std::ostringstream outstr;
  schema::PrintSchema(inodeSchema.get(), outstr);
  XLOGF(INFO, "inode schema: {}", outstr.str());
}

TEST(TestSerdeSchemaBuilder, DirEntry) {
  SerdeSchemaBuilder<meta::DirEntry> builder;
  auto dirEntrySchema = builder.getSchema();

  std::ostringstream outstr;
  schema::PrintSchema(dirEntrySchema.get(), outstr);
  XLOGF(INFO, "inode schema: {}", outstr.str());
}

}  // namespace hf3fs::analytics
