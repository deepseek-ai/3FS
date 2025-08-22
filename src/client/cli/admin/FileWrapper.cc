#include "FileWrapper.h"

#include "common/net/ib/RDMABuf.h"
#include "common/utils/Result.h"

namespace hf3fs::client::cli {
namespace {

auto rdmabufPool = net::RDMABufPool::create(256_MB, 1024);

}  // namespace

static constexpr std::array<uint8_t, 16_MB> zeros{};

Result<std::vector<Block>> FileWrapper::prepareBlocks(size_t offset, size_t end, const flat::RoutingInfo &routingInfo) {
  auto chunkSize = file().layout.chunkSize;
  std::vector<Block> blocks;
  while (offset < end) {
    auto chainResult = file().getChainId(inode_, offset, routingInfo, 0);
    RETURN_AND_LOG_ON_ERROR(chainResult);
    auto chunk = file().getChunkId(inode_.id, offset).then([](auto chunk) { return storage::ChunkId(chunk); });
    RETURN_AND_LOG_ON_ERROR(chunk);
    auto next = std::min((offset / chunkSize + 1) * chunkSize, end);
    blocks.push_back(Block{*chainResult, *chunk, offset % chunkSize, next - offset});
    offset = next;
  }
  return blocks;
}

Result<uint32_t> FileWrapper::replicasNum() {
  RETURN_AND_LOG_ON_ERROR(folly::coro::blockingWait(env_.mgmtdClientGetter()->refreshRoutingInfo(true)));
  auto routingInfo = env_.mgmtdClientGetter()->getRoutingInfo();
  if (routingInfo == nullptr || routingInfo->raw() == nullptr) {
    return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }

  auto chainIdResult = file().getChainId(inode_, 0, *routingInfo->raw(), 0);
  RETURN_AND_LOG_ON_ERROR(chainIdResult);
  auto chainId = *chainIdResult;

  auto chainResult = routingInfo->getChain(chainId);
  if (!chainResult) {
    return makeError(StorageClientCode::kRoutingError, fmt::format("chain info is null, {}", chainId));
  }

  uint32_t num = 0;
  for (auto &info : chainResult->targets) {
    num += info.publicState == flat::PublicTargetState::SERVING;
  }
  return num;
}

Result<Void> FileWrapper::showChunks(size_t offset, size_t length) {
  RETURN_AND_LOG_ON_ERROR(folly::coro::blockingWait(env_.mgmtdClientGetter()->refreshRoutingInfo(true)));
  auto routingInfo = env_.mgmtdClientGetter()->getRoutingInfo()->raw();
  if (routingInfo == nullptr) {
    return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }

  auto blocksResult = prepareBlocks(offset, offset + length, *routingInfo);
  RETURN_AND_LOG_ON_ERROR(blocksResult);
  auto &blocks = *blocksResult;
  auto idx = 0;
  for (auto &block : blocks) {
    if (idx == 10) {
      std::cout << "... (has more chunks)" << std::endl;
      break;
    }
    std::cout << fmt::format("block{}: {}", idx++, block) << std::endl;
  }
  return Void{};
}

CoTryTask<storage::ChecksumInfo> FileWrapper::readFile(std::ostream &out,
                                                       size_t length,
                                                       size_t offset,
                                                       bool checksum,
                                                       bool hex,
                                                       storage::client::TargetSelectionMode mode,
                                                       MD5_CTX *md5 /* = nullptr */,
                                                       bool fillZero /* = false */,
                                                       bool verbose /* = false */,
                                                       uint32_t targetIndex /* = 0 */) {
  auto routingInfo = env_.mgmtdClientGetter()->getRoutingInfo()->raw();
  if (routingInfo == nullptr) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }
  auto blocksResult = prepareBlocks(offset, offset + length, *routingInfo);
  CO_RETURN_AND_LOG_ON_ERROR(blocksResult);
  auto &blocks = *blocksResult;
  co_return co_await readFile(env_, out, blocks, checksum, hex, mode, md5, fillZero, verbose, targetIndex);
}

CoTryTask<storage::ChecksumInfo> FileWrapper::readFile(AdminEnv &env,
                                                       std::ostream &out,
                                                       const std::vector<Block> &blocks,
                                                       bool checksum,
                                                       bool hex,
                                                       storage::client::TargetSelectionMode mode,
                                                       MD5_CTX *md5 /* = nullptr */,
                                                       bool fillZero /* = false */,
                                                       bool verbose /* = false */,
                                                       uint32_t targetIndex /* = 0 */) {
  auto client = env.storageClientGetter();
  auto buffer = co_await rdmabufPool->allocate();
  if (UNLIKELY(!buffer)) {
    XLOGF(ERR, "allocate buffer failed");
    co_return makeError(RPCCode::kRDMANoBuf);
  }
  std::basic_string_view<uint8_t> data(buffer.ptr(), buffer.size());
  auto readBuffer = storage::client::IOBuffer{buffer};
  auto bufferOffset = 0;
  std::vector<storage::client::ReadIO> batch;
  storage::client::ReadOptions readOptions;
  readOptions.set_enableChecksum(checksum);
  readOptions.targetSelection().set_mode(mode);
  readOptions.targetSelection().set_targetIndex(targetIndex);

  storage::ChecksumInfo checksumInfo;
  for (auto &block : blocks) {
    auto readIO = client->createReadIO(block.chainId,
                                       block.chunkId,
                                       block.offset,
                                       block.length,
                                       (uint8_t *)&data[bufferOffset],
                                       &readBuffer);
    batch.push_back(std::move(readIO));
    bufferOffset += block.length;
    if (&block == &blocks.back() || bufferOffset + (&block + 1)->length > buffer.size()) {
      auto result = co_await client->batchRead(batch, env.userInfo, readOptions);
      CO_RETURN_AND_LOG_ON_ERROR(result);
      for (auto &readIO : batch) {
        auto succLength = 0ul;
        if (readIO.result.lengthInfo) {
          succLength = *readIO.result.lengthInfo;
        } else if (readIO.result.lengthInfo.error().code() == StorageClientCode::kChunkNotFound && fillZero) {
        } else {
          CO_RETURN_AND_LOG_ON_ERROR(readIO.result.lengthInfo);
        }
        if (succLength != readIO.length) {
          if (fillZero) {
            if (verbose) {
              std::cout << fmt::format("{} find hole, expected size {}, actual size {}\n",
                                       readIO.chunkId,
                                       readIO.length,
                                       succLength);
            }
            auto needFill = readIO.length - succLength;
            CO_RETURN_AND_LOG_ON_ERROR(readIO.result.checksum.combine(
                storage::ChecksumInfo::create(storage::ChecksumType::CRC32C, zeros.data(), needFill),
                needFill));
            succLength = readIO.length;
          } else {
            co_return makeError(StatusCode::kInvalidFormat,
                                fmt::format("read is short, chain {} chunk {} offset {} expect {} read {}",
                                            readIO.routingTarget.chainId,
                                            readIO.chunkId,
                                            readIO.offset,
                                            readIO.length,
                                            succLength));
          }
        }
        CO_RETURN_AND_LOG_ON_ERROR(checksumInfo.combine(readIO.result.checksum, succLength));
      }

      // clean up.
      if (hex) {
        out << fmt::format("{:02X}", fmt::join(data.substr(0, bufferOffset), " "));
      } else {
        out.write(reinterpret_cast<const char *>(data.data()), bufferOffset);
      }
      if (md5) {
        int ret = MD5_Update(md5, reinterpret_cast<const char *>(data.data()), bufferOffset);
        if (UNLIKELY(ret != 1)) {
          co_return makeError(StatusCode::kInvalidArg, fmt::format("md5 update error {}", ret));
        }
      }
      if (!out) {
        co_return makeError(StatusCode::kInvalidArg, "write to stream failed");
      }
      batch.clear();
      bufferOffset = 0;
    }
  }
  if (hex) {
    out << std::endl;
  }
  co_return checksumInfo;
}

CoTryTask<storage::ChecksumInfo> FileWrapper::writeFile(std::istream &in,
                                                        size_t length,
                                                        size_t offset,
                                                        Duration timeout,
                                                        bool syncAfterWrite /* = true */) {
  auto chunkSize = file().layout.chunkSize;
  auto client = env_.storageClientGetter();
  std::vector<uint8_t> data(64_MB, 0);
  auto registerResult = client->registerIOBuffer(&data[0], data.size());
  CO_RETURN_AND_LOG_ON_ERROR(registerResult);
  auto writeBuffer = std::move(*registerResult);
  auto bufferOffset = 0;
  std::vector<storage::client::WriteIO> batch;
  storage::client::WriteOptions writeOptions;
  writeOptions.retry().set_init_wait_time(timeout);
  writeOptions.retry().set_max_wait_time(timeout);
  writeOptions.retry().set_max_retry_time(timeout * 10);

  auto routingInfo = env_.mgmtdClientGetter()->getRoutingInfo()->raw();
  if (routingInfo == nullptr) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }

  auto blocksResult = prepareBlocks(offset, offset + length, *routingInfo);
  CO_RETURN_AND_LOG_ON_ERROR(blocksResult);
  auto &blocks = *blocksResult;

  storage::ChecksumInfo checksum;
  for (auto &block : blocks) {
    if (!in.read(reinterpret_cast<char *>(&data[bufferOffset]), block.length)) {
      co_return makeError(StatusCode::kInvalidArg, "write from file failed");
    }
    auto writeIO = client->createWriteIO(block.chainId,
                                         block.chunkId,
                                         block.offset,
                                         block.length,
                                         chunkSize,
                                         &data[bufferOffset],
                                         &writeBuffer);
    batch.push_back(std::move(writeIO));
    bufferOffset += block.length;
    if (bufferOffset + chunkSize > data.capacity() || &block == &blocks.back()) {
      auto result = co_await client->batchWrite(batch, env_.userInfo, writeOptions);
      CO_RETURN_AND_LOG_ON_ERROR(result);
      for (auto &writeIO : batch) {
        CO_RETURN_AND_LOG_ON_ERROR(writeIO.result.lengthInfo);
        auto succLength = *writeIO.result.lengthInfo;
        if (succLength != writeIO.length) {
          co_return makeError(StatusCode::kInvalidFormat,
                              fmt::format("write is short, chain {} chunk {} offset {} expect {} write {}",
                                          writeIO.routingTarget.chainId,
                                          writeIO.chunkId,
                                          writeIO.offset,
                                          writeIO.length,
                                          succLength));
        }
        CO_RETURN_AND_LOG_ON_ERROR(checksum.combine(writeIO.result.checksum, succLength));
      }

      // clean up.
      batch.clear();
      bufferOffset = 0;
    }
  }
  if (syncAfterWrite) {
    auto syncResult = co_await sync();
    CO_RETURN_AND_LOG_ON_ERROR(syncResult);
  }

  co_return checksum;
}

CoTryTask<FileWrapper> FileWrapper::openOrCreateFile(AdminEnv &env,
                                                     const Path &src,
                                                     bool createIsMissing /* = false */) {
  meta::Inode inode;
  auto statResult = co_await env.metaClientGetter()->stat(env.userInfo, env.currentDirId, src, true);
  if (!statResult && statResult.error().code() == MetaCode::kNotFound && createIsMissing) {
    auto result = co_await env.metaClientGetter()
                      ->create(env.userInfo, env.currentDirId, src, std::nullopt, meta::Permission(0644), 0);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    inode = *result;
  } else if (!statResult) {
    CO_RETURN_AND_LOG_ON_ERROR(statResult);
  } else {
    inode = *statResult;
  }
  if (!inode.isFile()) {
    co_return makeError(StatusCode::kInvalidArg, "not a file");
  }

  FileWrapper file(env);
  file.inode_ = std::move(inode);
  co_return file;
}

}  // namespace hf3fs::client::cli
