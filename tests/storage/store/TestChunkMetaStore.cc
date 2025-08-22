#include <chrono>
#include <thread>
#include <vector>

#include "common/utils/Size.h"
#include "storage/store/ChunkFileStore.h"
#include "storage/store/ChunkMetaStore.h"
#include "storage/store/PhysicalConfig.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage::test {
namespace {

using namespace std::chrono_literals;

TEST(TestChunkMetaStore, Normal) {
  folly::test::TemporaryDirectory tmpPath;

  kv::KVStore::Config config;
  PhysicalConfig targetConfig;
  targetConfig.path = tmpPath.path();
  targetConfig.physical_file_count = 8;
  targetConfig.chunk_size_list = {128_KB};

  GlobalFileStore globalFileStore;
  ChunkFileStore::Config fileStoreConfig;
  ChunkFileStore store(fileStoreConfig, globalFileStore);
  ASSERT_OK(store.create(targetConfig));

  auto chunkSize = targetConfig.chunk_size_list.front();
  ChunkMetaStore::Config metaConfig;
  ChunkMetaStore metaStore(metaConfig, store);
  ASSERT_OK(metaStore.create(config, targetConfig));

  const ChunkId chunkId{0xBeef, 0xBeef};

  {
    ChunkMetadata meta;
    auto result = metaStore.get(chunkId, meta);
    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().code(), StorageCode::kChunkMetadataNotFound);
  }

  {
    ChunkMetadata meta;
    meta.innerFileId.chunkSize = chunkSize;
    auto result = metaStore.set(chunkId, meta);
    ASSERT_OK(result);
  }

  {
    ChunkMetadata meta;
    auto result = metaStore.get(chunkId, meta);
    ASSERT_OK(result);
    ASSERT_EQ(meta.innerFileId.chunkSize, chunkSize);

    auto removeResult = metaStore.remove(chunkId, meta);
    ASSERT_OK(removeResult);
  }

  {
    ChunkMetadata meta;
    auto result = metaStore.get(chunkId, meta);
    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().code(), StorageCode::kChunkMetadataNotFound);
  }
}

TEST(TestChunkMetaStore, Iterator) {
  folly::test::TemporaryDirectory tmpPath;

  kv::KVStore::Config config;
  PhysicalConfig targetConfig;
  targetConfig.path = tmpPath.path();
  targetConfig.physical_file_count = 8;
  targetConfig.chunk_size_list = {128_KB};

  GlobalFileStore globalFileStore;
  ChunkFileStore::Config fileStoreConfig;
  ChunkFileStore store(fileStoreConfig, globalFileStore);
  ASSERT_OK(store.create(targetConfig));

  ChunkMetaStore::Config metaConfig;
  ChunkMetaStore metaStore(metaConfig, store);
  ASSERT_OK(metaStore.create(config, targetConfig));

  constexpr uint64_t P = 0xBeef;
  constexpr uint64_t N = 8;
  for (uint64_t i = 1; i <= N; ++i) {
    ChunkMetadata meta;
    meta.innerFileId.chunkSize = 128_KB;
    const ChunkId chunkId{P, i};
    auto result = metaStore.set(chunkId, meta);
    ASSERT_OK(result);
  }

  auto iterator = metaStore.iterator(ChunkId(P, 0).data().substr(0, 8));
  ASSERT_OK(iterator);
  for (uint64_t i = 0; i < N; ++i) {
    ASSERT_TRUE(iterator->valid());
    ASSERT_EQ(iterator->chunkId(), (ChunkId{P, N - i}));
    iterator->next();
  }
  ASSERT_FALSE(iterator->valid());
}

}  // namespace
}  // namespace hf3fs::storage::test
