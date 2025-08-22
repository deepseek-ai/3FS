#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>
#include <sstream>

#include "AdminEnv.h"
#include "WriteFile.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("fill-zero");
  parser.add_argument("path");
  parser.add_argument("--dry-run").default_value(false).implicit_value(true);
  parser.add_argument("--verbose").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleFillZero(IEnv &ienv,
                                                  const argparse::ArgumentParser &parser,
                                                  const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path src = parser.get<std::string>("path");
  auto dryRun = parser.get<bool>("--dry-run");
  auto verbose = parser.get<bool>("--verbose");
  auto rdmabufPool = net::RDMABufPool::create(32_MB, 1024);
  if (dryRun) {
    std::cout << "Dry-run mode, no data will be written" << std::endl;
  }

  // 2. stat file.
  auto openResult = co_await FileWrapper::openOrCreateFile(env, src, true);
  CO_RETURN_AND_LOG_ON_ERROR(openResult);
  auto &file = *openResult;

  auto storageClient = env.storageClientGetter();
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo()->raw();
  if (UNLIKELY(routingInfo == nullptr)) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }

  // 3. read and fill.
  storage::client::ReadOptions readOptions;
  storage::client::WriteOptions writeOptions;
  uint64_t chunkSize = file.file().layout.chunkSize;
  for (auto startPoint = 0ul; startPoint < file.length(); startPoint += 32_MB) {
    std::vector<storage::client::ReadIO> readIOs;
    std::vector<storage::client::WriteIO> writeIOs;
    net::RDMABuf current = co_await rdmabufPool->allocate();
    storage::client::IOBuffer buffer(current);

    for (auto offset = startPoint; offset < file.length() && offset < startPoint + 32_MB; offset += chunkSize) {
      auto chainResult = file.file().getChainId(file.inode(), offset, *routingInfo, 0);
      CO_RETURN_AND_LOG_ON_ERROR(chainResult);
      auto chunk =
          file.file().getChunkId(file.inode().id, offset).then([](auto chunk) { return storage::ChunkId(chunk); });
      CO_RETURN_AND_LOG_ON_ERROR(chunk);
      auto l = std::min(file.file().length - offset, chunkSize);
      readIOs.push_back(
          storageClient->createReadIO(*chainResult, *chunk, offset % chunkSize, l, current.ptr(), &buffer));
      current.advance(chunkSize);
    }

    auto readResult = co_await storageClient->batchRead(readIOs, env.userInfo, readOptions);
    CO_RETURN_AND_LOG_ON_ERROR(readResult);

    net::RDMABuf writeCurrent = co_await rdmabufPool->allocate();
    storage::client::IOBuffer writeBuffer(writeCurrent);
    std::memset(writeCurrent.ptr(), 0, 32_MB);
    for (auto &readIO : readIOs) {
      auto succLength = 0ul;
      if (readIO.result.lengthInfo) {
        succLength = *readIO.result.lengthInfo;
      } else if (readIO.result.lengthInfo.error().code() != StorageClientCode::kChunkNotFound) {
        CO_RETURN_AND_LOG_ON_ERROR(readIO.result.lengthInfo);
      }

      if (succLength < readIO.length) {
        if (verbose || dryRun) {
          std::cout << fmt::format("{} find hole, expected size {}, actual size {}\n",
                                   readIO.chunkId,
                                   readIO.length,
                                   succLength);
        }
        writeIOs.push_back(storageClient->createWriteIO(readIO.routingTarget.chainId,
                                                        readIO.chunkId,
                                                        succLength,
                                                        readIO.length - succLength,
                                                        chunkSize,
                                                        writeCurrent.ptr(),
                                                        &writeBuffer));
        writeCurrent.advance(chunkSize);
      }
    }

    if (!writeIOs.empty() && !dryRun) {
      auto writeResult = co_await storageClient->batchWrite(writeIOs, env.userInfo, writeOptions);
      CO_RETURN_AND_LOG_ON_ERROR(writeResult);
    }
  }
  co_return table;
}

}  // namespace

CoTryTask<void> registerFillZeroHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleFillZero);
}

}  // namespace hf3fs::client::cli
