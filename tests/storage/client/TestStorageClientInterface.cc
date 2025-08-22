#include <folly/experimental/coro/BlockingWait.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::client {
namespace {

using namespace hf3fs::test;

class TestStorageClientInterface : public UnitTestFabric, public ::testing::TestWithParam<SystemSetupConfig> {
 protected:
  TestStorageClientInterface()
      : UnitTestFabric(GetParam()) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());

    clientConfig_.retry().set_init_wait_time(30_s);
    clientConfig_.retry().set_max_wait_time(30_s);
    clientConfig_.retry().set_max_retry_time(30_s);
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_P(TestStorageClientInterface, Write) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0xFF);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);

  // create write IO

  auto ioBuffer = std::move(*regRes);
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);

  for (bool bypassDiskIO : {true, false}) {
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);

    WriteOptions options;
    options.debug().set_bypass_disk_io(bypassDiskIO);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));

    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  {
    QueryChunkReq req;
    req.chainId = firstChainId_;
    req.chunkId = chunkId;
    auto result = folly::coro::blockingWait(storageClient_->queryChunk(req));
    ASSERT_OK(result);
    XLOGF(INFO, "chunk info {}", serde::toJsonString(*result, false, true));
  }
}

TEST_P(TestStorageClientInterface, BatchWrite) {
  // register a block of memory
  size_t numWriteIOs = 3;
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size() * numWriteIOs, 0xFF);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);

  // create write IOs

  auto ioBuffer = std::move(*regRes);

  for (bool bypassDiskIO : {true, false}) {
    std::vector<WriteIO> writeIOs;

    for (size_t writeIndex = 0; writeIndex < numWriteIOs; writeIndex++) {
      auto chainId = firstChainId_;
      ChunkId chunkId(1 /*high*/, writeIndex /*low*/);
      auto writeIO = storageClient_->createWriteIO(chainId,
                                                   chunkId,
                                                   0 /*offset*/,
                                                   setupConfig_.chunk_size() /*length*/,
                                                   setupConfig_.chunk_size() /*chunkSize*/,
                                                   &memoryBlock[writeIndex * setupConfig_.chunk_size()],
                                                   &ioBuffer);
      writeIOs.push_back(std::move(writeIO));
    }

    WriteOptions options;
    options.debug().set_bypass_disk_io(bypassDiskIO);
    folly::coro::blockingWait(storageClient_->batchWrite(writeIOs, flat::UserInfo(), options));

    for (const auto &writeIO : writeIOs) {
      ASSERT_OK(writeIO.result.lengthInfo);
      ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
    }
  }
}

TEST_P(TestStorageClientInterface, Read) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);

  // create read IO

  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_OK(ioResult.lengthInfo);
  ASSERT_EQ(chunkData.size(), ioResult.lengthInfo.value());

  auto ioBuffer = std::move(*regRes);
  auto readIO = storageClient_->createReadIO(chainId,
                                             chunkId /*chunkId*/,
                                             0 /*offset*/,
                                             setupConfig_.chunk_size() /*length*/,
                                             &memoryBlock[0],
                                             &ioBuffer);

  ReadOptions options;

  for (bool bypassDiskIO : {true, false}) {
    options.debug().set_bypass_disk_io(bypassDiskIO);
    folly::coro::blockingWait(storageClient_->read(readIO, flat::UserInfo(), options));

    ASSERT_OK(readIO.result.lengthInfo);
    ASSERT_EQ(readIO.length, readIO.result.lengthInfo.value());

    if (!bypassDiskIO) {
      for (size_t i = 0; i < chunkData.size(); i++) ASSERT_EQ(chunkData[i], memoryBlock[i]);
    }
  };
}

TEST_P(TestStorageClientInterface, BatchRead) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);

  // create read IOs

  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_OK(ioResult.lengthInfo);
  ASSERT_EQ(chunkData.size(), ioResult.lengthInfo.value());

  auto ioBuffer = std::move(*regRes);
  std::vector<ReadIO> readIOs;

  for (size_t offset = 0; offset < setupConfig_.chunk_size(); offset += setupConfig_.chunk_size() / 4) {
    auto readIO = storageClient_->createReadIO(chainId,
                                               chunkId,
                                               offset /*offset*/,
                                               setupConfig_.chunk_size() / 4 /*length*/,
                                               &memoryBlock[offset],
                                               &ioBuffer);
    readIOs.push_back(std::move(readIO));
  }

  ReadOptions options;

  for (bool bypassDiskIO : {true, false}) {
    options.debug().set_bypass_disk_io(bypassDiskIO);
    folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo(), options));

    for (const auto &readIO : readIOs) {
      ASSERT_OK(readIO.result.lengthInfo);
      ASSERT_EQ(readIO.length, readIO.result.lengthInfo.value());
    }

    if (!bypassDiskIO) {
      for (size_t i = 0; i < chunkData.size(); i++) ASSERT_EQ(chunkData[i], memoryBlock[i]);
    }
  }
}

TEST_P(TestStorageClientInterface, BatchReadUnaligned) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);

  // create read IOs

  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
  for (auto &ch : chunkData) {
    ch = rand() & 0xFF;
  }

  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_OK(ioResult.lengthInfo);
  ASSERT_EQ(chunkData.size(), ioResult.lengthInfo.value());

  auto ioBuffer = std::move(*regRes);
  std::vector<ReadIO> readIOs;

  for (size_t offset = 0; offset < setupConfig_.chunk_size(); offset += setupConfig_.chunk_size() / 4) {
    auto headSize = 1 + rand() % 17;
    auto tailSize = 1 + rand() % 17;
    auto unalignedOffset = offset + headSize;
    auto unalignedLength = setupConfig_.chunk_size() / 4 - headSize - tailSize;
    XLOGF(INFO,
          "Unaligned offset {} length {}, chunk size {} chunk id {}",
          unalignedOffset,
          unalignedLength,
          setupConfig_.chunk_size(),
          chunkId);
    auto readIO = storageClient_->createReadIO(chainId,
                                               chunkId,
                                               unalignedOffset /*offset*/,
                                               unalignedLength /*length*/,
                                               &memoryBlock[unalignedOffset],
                                               &ioBuffer);
    readIOs.push_back(std::move(readIO));
  }

  ReadOptions options;

  for (bool bypassDiskIO : {true, false}) {
    options.debug().set_bypass_disk_io(bypassDiskIO);
    folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo(), options));

    for (const auto &readIO : readIOs) {
      ASSERT_OK(readIO.result.lengthInfo);
      ASSERT_EQ(readIO.length, readIO.result.lengthInfo.value());
      if (!bypassDiskIO) {
        ASSERT_TRUE(std::memcmp(&chunkData[readIO.offset], &memoryBlock[readIO.offset], readIO.length) == 0);
      }
    }
  }
}

TEST_P(TestStorageClientInterface, ReadWrite) {
  // register a block of memory
  std::vector<uint8_t> writeData(setupConfig_.chunk_size() / 2, 0xFF);
  auto regWriteDataRes = storageClient_->registerIOBuffer(&writeData[0], writeData.size());
  ASSERT_OK(regWriteDataRes);

  auto writeBuffer = std::move(*regWriteDataRes);
  ChainId chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);

  // write data to chunk
  auto writeIO = storageClient_->createWriteIO(chainId,
                                               chunkId,
                                               0 /*offset*/,
                                               writeData.size() /*length*/,
                                               setupConfig_.chunk_size(),
                                               &writeData[0],
                                               &writeBuffer);

  folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo()));

  ASSERT_OK(writeIO.result.lengthInfo);
  ASSERT_EQ(writeData.size(), writeIO.result.lengthInfo.value());

  // read the data back
  std::vector<uint8_t> readData(setupConfig_.chunk_size(), 0);
  auto regReadDataRes = storageClient_->registerIOBuffer(&readData[0], readData.size());
  ASSERT_OK(regReadDataRes);

  auto readBuffer = std::move(*regReadDataRes);
  auto readIO = storageClient_->createReadIO(chainId,
                                             chunkId,
                                             0 /*offset*/,
                                             setupConfig_.chunk_size() /*length*/,
                                             &readData[0],
                                             &readBuffer);

  folly::coro::blockingWait(storageClient_->read(readIO, flat::UserInfo()));

  ASSERT_OK(readIO.result.lengthInfo);
  ASSERT_EQ(writeIO.result.lengthInfo.value(), readIO.result.lengthInfo.value());
  for (size_t i = 0; i < writeData.size(); i++) ASSERT_EQ(writeData[i], readData[i]);
}

TEST_P(TestStorageClientInterface, BatchReadWrite) {
  ChainId chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);

  // write data to chunk
  size_t numChunks = 3;
  std::vector<uint8_t> writeData(setupConfig_.chunk_size() * numChunks, 0xFF);
  auto regWriteDataRes = storageClient_->registerIOBuffer(&writeData[0], writeData.size());
  ASSERT_OK(regWriteDataRes);
  auto writeBuffer = std::move(*regWriteDataRes);

  std::vector<WriteIO> writeIOs;

  for (size_t writeIndex = 0; writeIndex < numChunks; writeIndex++) {
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 ChunkId(chunkId, writeIndex),
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size(),
                                                 &writeData[writeIndex * setupConfig_.chunk_size()],
                                                 &writeBuffer);
    writeIOs.push_back(std::move(writeIO));
  }

  folly::coro::blockingWait(storageClient_->batchWrite(writeIOs, flat::UserInfo()));

  for (const auto &writeIO : writeIOs) {
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(setupConfig_.chunk_size(), writeIO.result.lengthInfo.value());
  }

  // read the data back
  std::vector<uint8_t> readData(setupConfig_.chunk_size() * numChunks, 0);
  auto regReadDataRes = storageClient_->registerIOBuffer(&readData[0], readData.size());
  ASSERT_OK(regReadDataRes);
  auto readBuffer = std::move(*regReadDataRes);

  std::vector<ReadIO> readIOs;

  for (size_t readIndex = 0; readIndex < numChunks; readIndex++) {
    auto readIO = storageClient_->createReadIO(chainId,
                                               ChunkId(chunkId, readIndex),
                                               0 /*offset*/,
                                               setupConfig_.chunk_size() /*length*/,
                                               &readData[readIndex * setupConfig_.chunk_size()],
                                               &readBuffer);
    readIOs.push_back(std::move(readIO));
  }

  folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo()));

  for (const auto &readIO : readIOs) {
    ASSERT_OK(readIO.result.lengthInfo);
    ASSERT_EQ(setupConfig_.chunk_size(), readIO.result.lengthInfo.value());
  }

  for (size_t i = 0; i < writeData.size(); i++) ASSERT_EQ(writeData[i], readData[i]) << "i " << i;
}

TEST_P(TestStorageClientInterface, VerifyChecksum) {
  if (setupConfig_.client_impl_type() == StorageClient::ImplementationType::InMem) return;

  ChainId chainId = firstChainId_;

  std::vector<uint8_t> readData(setupConfig_.chunk_size(), 0);
  std::vector<uint8_t> writeData(setupConfig_.chunk_size(), 0xFF);

  auto regReadBuf = storageClient_->registerIOBuffer(&readData[0], readData.size());
  ASSERT_OK(regReadBuf);
  auto readBuffer = std::move(*regReadBuf);

  auto regWriteBuf = storageClient_->registerIOBuffer(&writeData[0], writeData.size());
  ASSERT_OK(regWriteBuf);
  auto writeBuffer = std::move(*regWriteBuf);

  // enable checksum for read/write IO
  clientConfig_.set_chunk_checksum_type(ChecksumType::CRC32C);
  client::WriteOptions writeOptions;
  writeOptions.set_enableChecksum(true);
  client::ReadOptions readOptions;
  readOptions.set_enableChecksum(true);
  readOptions.targetSelection().set_mode(TargetSelectionMode::RoundRobin);

  enum WritePattern {
    SEQWRITE = 1,
    JUMPWRITE,
    RANDWRITE,
  };

  for (WritePattern pattern : {SEQWRITE, JUMPWRITE, RANDWRITE}) {
    ChunkId chunkId(1 /*high*/, pattern /*low*/);
    std::vector<uint8_t> chunkData;
    size_t offset = 0;
    size_t length = 0;

    for (size_t writeIndex = 1; writeIndex <= 100; writeIndex++) {
      switch (pattern) {
        case SEQWRITE:
          offset += length;
          break;
        case JUMPWRITE:
          offset += length + folly::Random::rand64(0, length / 2);
          break;
        case RANDWRITE:
          offset = folly::Random::rand64(0, setupConfig_.chunk_size());
          break;
      }

      if (offset + 1 >= setupConfig_.chunk_size()) continue;
      length = folly::Random::rand64(1, (setupConfig_.chunk_size() - offset) / 2);

      XLOGF(INFO,
            "Verify {} checksum #{}: offset {} length {} chunk size {}",
            magic_enum::enum_name(pattern),
            writeIndex,
            offset,
            length,
            chunkData.size());

      // generate random chunk data
      for (size_t byteIndex = 0; byteIndex + sizeof(uint64_t) < length; byteIndex += sizeof(uint64_t)) {
        auto dataPtr = reinterpret_cast<uint64_t *>(&writeData[byteIndex]);
        *dataPtr = folly::Random::rand64();
      }

      // write to a random offset in the chunk
      auto writeIO =
          storageClient_
              ->createWriteIO(chainId, chunkId, offset, length, setupConfig_.chunk_size(), &writeData[0], &writeBuffer);
      folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), writeOptions));
      ASSERT_RESULT_EQ(writeIO.length, writeIO.result.lengthInfo);

      // verify checksum of the write data
      ASSERT_EQ(folly::crc32c(&writeData[0], *writeIO.result.lengthInfo), writeIO.localChecksum().value);
      // update chunk data and compute the chunk checksum
      if (offset + length > chunkData.size()) chunkData.resize(offset + length);
      std::memcpy(&chunkData[offset], &writeData[0], *writeIO.result.lengthInfo);
      ASSERT_EQ(folly::crc32c(&chunkData[0], chunkData.size()), writeIO.result.checksum.value);

      {
        // read back the write data
        auto readIO = storageClient_->createReadIO(chainId, chunkId, offset, length, &readData[0], &readBuffer);
        folly::coro::blockingWait(storageClient_->read(readIO, flat::UserInfo(), readOptions));
        ASSERT_RESULT_EQ(readIO.length, readIO.result.lengthInfo);

        // verify checksum of the read data
        ASSERT_EQ(folly::crc32c(&readData[0], *readIO.result.lengthInfo), readIO.result.checksum.value);
        // compare with checksum of write data
        ASSERT_EQ(writeIO.localChecksum().value, readIO.result.checksum.value);
      }

      {
        // read the entire chunk
        auto readIO =
            storageClient_->createReadIO(chainId, chunkId, 0, setupConfig_.chunk_size(), &readData[0], &readBuffer);
        folly::coro::blockingWait(storageClient_->read(readIO, flat::UserInfo(), readOptions));
        ASSERT_OK(readIO.result.lengthInfo);

        // verify checksum of the read data
        ASSERT_EQ(folly::crc32c(&readData[0], *readIO.result.lengthInfo), readIO.result.checksum.value);
        // compare with chunk checksum
        ASSERT_EQ(writeIO.result.checksum.value, readIO.result.checksum.value);
      }
    }
  }
}

TEST_P(TestStorageClientInterface, QueryRemoveTruncateChunks) {
  ChainId chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 0 /*low*/);
  const uint32_t maxNumResultsPerQuery = serverConfigs_.front().storage().max_num_results_per_query();

  for (uint32_t numChunks : {1U,
                             maxNumResultsPerQuery - 1,
                             maxNumResultsPerQuery,
                             maxNumResultsPerQuery + 1,
                             2 * maxNumResultsPerQuery - 2,
                             2 * maxNumResultsPerQuery,
                             2 * maxNumResultsPerQuery + 2,
                             folly::Random::rand32(1, 3 * maxNumResultsPerQuery)}) {
    uint32_t writeLen = setupConfig_.chunk_size() / 2;
    std::vector<uint8_t> writeData(writeLen, 0xFF);

    auto result = writeToChunks(chainId, ChunkId(1, 0), ChunkId(1, numChunks), writeData);
    ASSERT_TRUE(result);

    {
      // truncate chunks
      std::vector<TruncateChunkOp> truncateOps;
      for (uint32_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
        auto op = storageClient_->createTruncateOp(chainId,
                                                   ChunkId(chunkId, chunkIndex),
                                                   setupConfig_.chunk_size(),
                                                   setupConfig_.chunk_size());
        truncateOps.push_back(std::move(op));
      }

      folly::coro::blockingWait(storageClient_->truncateChunks(truncateOps, flat::UserInfo()));

      for (const auto &op : truncateOps) {
        ASSERT_OK(op.result.lengthInfo);
        ASSERT_EQ(op.chunkLen, op.result.lengthInfo.value());
      }
    }

    {
      // read chunks created by truncate ops
      for (uint32_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
        std::vector<uint8_t> readData(setupConfig_.chunk_size(), 0x0);
        auto result = readFromChunk(chainId, ChunkId(chunkId, chunkIndex), readData);
        ASSERT_OK(result.lengthInfo);
        ASSERT_EQ(setupConfig_.chunk_size(), result.lengthInfo.value());

        for (size_t index = 0; index < readData.size(); index++) {
          if (index < writeLen)
            ASSERT_EQ(0xFF, readData[index]);  // the written part
          else
            ASSERT_EQ(0x00, readData[index]);  // the extended part
        }
      }
    }

    {
      // query only last chunk
      auto queryOp = storageClient_->createQueryOp(chainId,
                                                   ChunkId(chunkId, 0),
                                                   ChunkId(chunkId, numChunks),
                                                   1 /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));

      ASSERT_OK(queryOp.result.statusCode);
      ASSERT_EQ(ChunkId(chunkId, numChunks - 1).data(), queryOp.result.lastChunkId.data());
      ASSERT_EQ(setupConfig_.chunk_size(), queryOp.result.lastChunkLen);
      ASSERT_EQ(setupConfig_.chunk_size(), queryOp.result.totalChunkLen);
      ASSERT_EQ(1, queryOp.result.totalNumChunks);
      ASSERT_TRUE(queryOp.result.moreChunksInRange || numChunks <= 1);
    }

    if (numChunks > 1) {
      // query half of the chunks
      auto halfNumChunks = numChunks / 2;
      auto queryOp = storageClient_->createQueryOp(chainId,
                                                   ChunkId(chunkId, 0),
                                                   ChunkId(chunkId, numChunks),
                                                   halfNumChunks /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));

      ASSERT_OK(queryOp.result.statusCode);
      ASSERT_EQ(setupConfig_.chunk_size(), queryOp.result.lastChunkLen);
      ASSERT_EQ(setupConfig_.chunk_size() * halfNumChunks, queryOp.result.totalChunkLen);
      ASSERT_EQ(halfNumChunks, queryOp.result.totalNumChunks);
      ASSERT_TRUE(queryOp.result.moreChunksInRange);
    }

    {
      // query all chunks
      auto queryOp = storageClient_->createQueryOp(chainId,
                                                   ChunkId(chunkId, 0),
                                                   ChunkId(chunkId, numChunks),
                                                   numChunks /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));

      ASSERT_OK(queryOp.result.statusCode);
      ASSERT_EQ(ChunkId(chunkId, numChunks - 1).data(), queryOp.result.lastChunkId.data());
      ASSERT_EQ(setupConfig_.chunk_size(), queryOp.result.lastChunkLen);
      ASSERT_EQ(setupConfig_.chunk_size() * numChunks, queryOp.result.totalChunkLen);
      ASSERT_EQ(numChunks, queryOp.result.totalNumChunks);
      ASSERT_FALSE(queryOp.result.moreChunksInRange);
    }

    {
      // query the max range
      auto queryOp = storageClient_->createQueryOp(chainId,
                                                   ChunkId(chunkId, 0),
                                                   ChunkId(chunkId, UINT32_MAX),
                                                   UINT32_MAX /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));

      ASSERT_OK(queryOp.result.statusCode);
      ASSERT_EQ(setupConfig_.chunk_size(), queryOp.result.lastChunkLen);
      ASSERT_EQ(setupConfig_.chunk_size() * numChunks, queryOp.result.totalChunkLen);
      ASSERT_EQ(numChunks, queryOp.result.totalNumChunks);
      ASSERT_FALSE(queryOp.result.moreChunksInRange);
    }

    {
      // remove the last chunk
      auto removeOp = storageClient_->createRemoveOp(chainId,
                                                     ChunkId(chunkId, 0),
                                                     ChunkId(chunkId, numChunks),
                                                     1 /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeOp, 1), flat::UserInfo()));

      ASSERT_OK(removeOp.result.statusCode);
      ASSERT_EQ(1, removeOp.result.numChunksRemoved);
      ASSERT_TRUE(removeOp.result.moreChunksInRange || numChunks <= 1);
    }

    {
      // remove the remaining chunks
      auto removeOp = storageClient_->createRemoveOp(chainId,
                                                     ChunkId(chunkId, 0),
                                                     ChunkId(chunkId, numChunks),
                                                     numChunks /*maxNumChunkIdsToProcess*/);

      folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeOp, 1), flat::UserInfo()));

      ASSERT_OK(removeOp.result.statusCode);
      ASSERT_EQ(numChunks - 1, removeOp.result.numChunksRemoved);
      ASSERT_FALSE(removeOp.result.moreChunksInRange);
    }

    {
      // check if chunks are removed
      auto queryOp = storageClient_->createQueryOp(chainId, ChunkId(chunkId, 0), ChunkId(chunkId, numChunks));

      folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));

      ASSERT_TRUE(queryOp.result.statusCode);
      ASSERT_EQ(ChunkId().data(), queryOp.result.lastChunkId.data());
      ASSERT_EQ(0, queryOp.result.lastChunkLen);
      ASSERT_EQ(0, queryOp.result.totalChunkLen);
      ASSERT_EQ(0, queryOp.result.totalNumChunks);
      ASSERT_FALSE(queryOp.result.moreChunksInRange);
    }
  }
}

TEST_P(TestStorageClientInterface, TruncateExtendChunks) {
  ChainId chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 0 /*low*/);
  uint32_t numChunks = 3;
  uint32_t writeLen = setupConfig_.chunk_size() / 2;

  std::vector<uint8_t> writeData(writeLen, 0xFF);
  auto result = writeToChunks(chainId, ChunkId(1, 0), ChunkId(1, numChunks), writeData);
  ASSERT_TRUE(result);

  {
    // extend op does not reduce chunk length
    std::vector<TruncateChunkOp> truncateOps;
    for (uint32_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
      auto op = storageClient_->createTruncateOp(chainId,
                                                 ChunkId(chunkId, chunkIndex),
                                                 writeLen / 2 /* try to reduce chunk length */,
                                                 setupConfig_.chunk_size(),
                                                 true /*onlyExtendChunk*/);
      truncateOps.push_back(std::move(op));
    }

    folly::coro::blockingWait(storageClient_->truncateChunks(truncateOps, flat::UserInfo()));

    for (const auto &op : truncateOps) {
      ASSERT_OK(op.result.lengthInfo);
      ASSERT_EQ(writeLen, op.result.lengthInfo.value());  // still get the original length
    }
  }

  {
    // extend to full chunks
    std::vector<TruncateChunkOp> truncateOps;
    for (uint32_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
      auto op = storageClient_->createTruncateOp(chainId,
                                                 ChunkId(chunkId, chunkIndex),
                                                 setupConfig_.chunk_size() /* full chunk size */,
                                                 setupConfig_.chunk_size(),
                                                 true /*onlyExtendChunk*/);
      truncateOps.push_back(std::move(op));
    }

    folly::coro::blockingWait(storageClient_->truncateChunks(truncateOps, flat::UserInfo()));

    for (const auto &op : truncateOps) {
      ASSERT_OK(op.result.lengthInfo);
      ASSERT_EQ(setupConfig_.chunk_size(), op.result.lengthInfo.value());
    }
  }

  {
    // read the extended chunks
    for (uint32_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
      std::vector<uint8_t> readData(setupConfig_.chunk_size(), 0xAA);
      auto result = readFromChunk(chainId, ChunkId(chunkId, chunkIndex), readData);
      ASSERT_OK(result.lengthInfo);
      ASSERT_EQ(setupConfig_.chunk_size(), result.lengthInfo.value());

      for (size_t index = 0; index < readData.size(); index++) {
        if (index < writeLen)
          ASSERT_EQ(0xFF, readData[index]);  // the written part
        else
          ASSERT_EQ(0x00, readData[index]);  // the extended part
      }
    }
  }
}

TEST_P(TestStorageClientInterface, QuerySpaceInfo) {
  auto result = folly::coro::blockingWait(storageClient_->querySpaceInfo(NodeId{1}));
  ASSERT_OK(result);
  ASSERT_TRUE(!result->spaceInfos.empty());
}

TEST_P(TestStorageClientInterface, CreateTarget) {
  CreateTargetReq req;
  req.targetId = TargetId{255};
  req.chainId = ChainId{255};
  req.diskIndex = 0;
  req.onlyChunkEngine = true;
  auto result = folly::coro::blockingWait(storageClient_->createTarget(NodeId{1}, req));
  ASSERT_OK(result);
}

TEST_P(TestStorageClientInterface, GetAllChunkMetadata) {
  ChainId chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 0 /*low*/);
  uint32_t numChunks = 3;

  std::vector<uint8_t> writeData(setupConfig_.chunk_size(), 0xFF);
  auto result = writeToChunks(chainId, chunkId, ChunkId(chunkId, numChunks), writeData);
  ASSERT_TRUE(result);

  auto routingInfo = getRoutingInfo();
  ASSERT_TRUE(routingInfo);
  auto chainInfo = routingInfo->getChain(chainId);
  ASSERT_TRUE(chainInfo);

  for (const auto targetInfo : chainInfo->targets) {
    auto chunkMetaVec = folly::coro::blockingWait(storageClient_->getAllChunkMetadata(chainId, targetInfo.targetId));
    ASSERT_OK(chunkMetaVec);

    std::set<ChunkId> uniqChunkIds;
    for (const auto &chunkMeta : *chunkMetaVec) {
      ASSERT_EQ(ChunkVer{1}, chunkMeta.commitVer);
      ASSERT_EQ(setupConfig_.chunk_size(), chunkMeta.length);
      uniqChunkIds.insert(chunkMeta.chunkId);
    }

    ASSERT_EQ(numChunks, uniqChunkIds.size());
    ASSERT_EQ(chunkId, *uniqChunkIds.begin());
    ASSERT_EQ(ChunkId(chunkId, numChunks - 1), *uniqChunkIds.rbegin());
  }
}

SystemSetupConfig testInMemClient = {
    128_KB /*chunkSize*/,
    1 /*numChains*/,
    1 /*numReplicas*/,
    1 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
    hf3fs::Path() /*clientConfig*/,
    hf3fs::Path() /*serverConfig*/,
    {} /*storageEndpoints*/,
    0 /*serviceLevel*/,
    0 /*listenPort*/,
    StorageClient::ImplementationType::InMem /*clientImplType*/,
};

SystemSetupConfig testRpcClient = {
    128_KB /*chunkSize*/,
    1 /*numChains*/,
    2 /*numReplicas*/,
    2 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
};

SystemSetupConfig testSmallChunk = {
    512 /*chunkSize*/,
    1 /*numChains*/,
    2 /*numReplicas*/,
    2 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
};

INSTANTIATE_TEST_SUITE_P(InMemClient,
                         TestStorageClientInterface,
                         ::testing::Values(testInMemClient),
                         SystemSetupConfig::prettyPrintConfig);
INSTANTIATE_TEST_SUITE_P(RpcClient,
                         TestStorageClientInterface,
                         ::testing::Values(testRpcClient),
                         SystemSetupConfig::prettyPrintConfig);
INSTANTIATE_TEST_SUITE_P(SmallChunk,
                         TestStorageClientInterface,
                         ::testing::Values(testSmallChunk),
                         SystemSetupConfig::prettyPrintConfig);

}  // namespace
}  // namespace hf3fs::storage::client
