#include <boost/filesystem/operations.hpp>
#include <thread>

#include "common/serde/Serde.h"
#include "common/utils/FileUtils.h"
#include "kv/KVStore.h"
#include "kv/MemDBStore.h"
#include "storage/service/StorageServer.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/Helper.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::test {
namespace {

using namespace hf3fs::test;

class TestDumpMeta : public UnitTestFabric, public ::testing::Test {
 protected:
  TestDumpMeta()
      : UnitTestFabric(SystemSetupConfig{128_KB /*chunkSize*/,
                                         1 /*numChains*/,
                                         1 /*numReplicas*/,
                                         1 /*numStorageNodes*/,
                                         {folly::fs::temp_directory_path()} /*dataPaths*/,
                                         hf3fs::Path() /*clientConfig*/,
                                         hf3fs::Path() /*serverConfig*/,
                                         {} /*storageEndpoints*/,
                                         0 /*serviceLevel*/,
                                         0 /*listenPort*/,
                                         client::StorageClient::ImplementationType::RPC,
                                         kv::KVStore::Type::RocksDB,
                                         false,
                                         true}) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_F(TestDumpMeta, Normal) {
  auto chunkId = ChunkId{0u, 0u};
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
  auto writeRes = writeToChunk(chainIds_.front(), chunkId, chunkData);
  ASSERT_OK(writeRes.lengthInfo);

  folly::test::TemporaryDirectory tmpPath;
  serverConfigs_[0].dump_worker().set_dump_root_path(tmpPath.path());
  serverConfigs_[0].dump_worker().set_dump_interval(100_ms);

  std::this_thread::sleep_for(2_s);
  stopAndRemoveStorageServer(0);

  std::vector<Path> files;
  for (auto &filePath : boost::filesystem::recursive_directory_iterator(tmpPath.path())) {
    if (boost::filesystem::is_regular_file(filePath)) {
      files.push_back(filePath);
    }
  }
  ASSERT_EQ(files.size(), 1);
  XLOGF(CRITICAL, "dump files: {}", serde::toJsonString(files));

  auto readResult = loadFile(files.front());
  ASSERT_OK(readResult);

  std::map<ChunkId, ChunkMetadata> metas;
  ASSERT_TRUE(serde::deserialize(metas, *readResult));
  ASSERT_EQ(metas.size(), 1);
  ASSERT_EQ(metas.begin()->first, chunkId);
  XLOGF(CRITICAL, "dump metas: {}", serde::toJsonString(metas));
}

}  // namespace
}  // namespace hf3fs::storage::test
