#include "client/cli/admin/ReadBench.h"

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Invoke.h>
#include <fstream>
#include <sstream>
#include <thread>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "client/storage/TargetSelection.h"
#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("read-bench");
  parser.add_argument("path");
  parser.add_argument("--threads").default_value(uint32_t{16}).scan<'u', uint32_t>();
  parser.add_argument("--coroutines").default_value(uint32_t{1}).scan<'u', uint32_t>();
  parser.add_argument("--seconds").default_value(uint32_t{60}).scan<'u', uint32_t>();
  parser.add_argument("--write").default_value(false).implicit_value(true);
  parser.add_argument("--bs");
  parser.add_argument("--iodepth");
  parser.add_argument("--mode");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path path = parser.get<std::string>("path");
  auto threads = parser.get<uint32_t>("--threads");
  auto coroutines = parser.get<uint32_t>("--coroutines");
  auto deadline = RelativeTime::now() + 1_s * parser.get<uint32_t>("--seconds");
  auto modeStr = parser.present<std::string>("--mode").value_or("Default");
  auto mode = magic_enum::enum_cast<storage::client::TargetSelectionMode>(modeStr);
  auto isWrite = parser.get<bool>("--write");
  if (!mode) {
    co_return makeError(StorageClientCode::kRoutingError, "mode is invalid");
  }
  size_t blockSize = 4_KB;
  if (auto p = parser.present<std::string>("--bs")) {
    auto result = Size::from(*p);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    blockSize = *result;
  }
  size_t iodepth = 1024;
  if (auto p = parser.present<std::string>("--iodepth")) {
    auto result = Size::from(*p);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    iodepth = *result;
  }
  auto pool = std::make_unique<folly::CPUThreadPoolExecutor>(std::make_pair(threads, threads),
                                                             std::make_shared<folly::NamedThreadFactory>("Pool"));

  // 2. open files.
  std::string prev;
  std::vector<FileWrapper> files;
  while (true) {
    auto res = co_await env.metaClientGetter()->list(env.userInfo, env.currentDirId, path, prev, 0, false);
    CO_RETURN_AND_LOG_ON_ERROR(res);
    const auto &lsp = res.value();
    for (const auto &entry : lsp.entries) {
      if (entry.isFile()) {
        auto openResult = co_await FileWrapper::openOrCreateFile(env, path / entry.name);
        CO_RETURN_AND_LOG_ON_ERROR(openResult);
        files.push_back(std::move(*openResult));
      }
    }
    if (lsp.more) {
      prev = lsp.entries.at(lsp.entries.size() - 1).name;
    } else {
      break;
    }
  }

  // 3. read.
  auto storageClient = env.storageClientGetter();
  std::vector<CoTryTask<Void>> total;
  std::atomic<size_t> readBytes{};
  total.reserve(coroutines);
  co_await env.mgmtdClientGetter()->refreshRoutingInfo(true);
  auto rdmabufPool = net::RDMABufPool::create(64_MB, 512);
  for (auto i = 0u; i < coroutines; ++i) {
    total.push_back(folly::coro::co_invoke([&]() -> CoTryTask<Void> {
      std::ofstream out("/dev/null");
      std::vector<storage::client::ReadIO> readIOs;
      std::vector<storage::client::WriteIO> writeIOs;
      std::vector<storage::client::IOBuffer> buffers;
      net::RDMABuf current;
      storage::client::ReadOptions readOptions;
      storage::client::WriteOptions writeOptions;
      readOptions.targetSelection().set_mode(*mode);
      readIOs.reserve(1024);
      writeIOs.reserve(1024);
      buffers.reserve(1024);
      while (RelativeTime::now() <= deadline) {
        readIOs.clear();
        writeIOs.clear();
        buffers.clear();
        current = {};
        auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo()->raw();
        if (UNLIKELY(routingInfo == nullptr)) {
          co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
        }

        for (auto i = 0u; i < iodepth; ++i) {
          auto &file = files[folly::Random::rand32() % files.size()];
          if (UNLIKELY(file.length() == 0)) {
            continue;
          }
          auto chunkSize = file.file().layout.chunkSize;
          auto offset = folly::Random::rand32() * blockSize % file.length();
          auto chainResult = file.file().getChainId(file.inode(), offset, *routingInfo, 0);
          CO_RETURN_AND_LOG_ON_ERROR(chainResult);
          auto chunk =
              file.file().getChunkId(file.inode().id, offset).then([](auto chunk) { return storage::ChunkId(chunk); });
          CO_RETURN_AND_LOG_ON_ERROR(chunk);
          auto l = std::min(file.file().length - offset, blockSize);
          if (current.size() < l) {
            current = co_await rdmabufPool->allocate();
            if (UNLIKELY(!current)) {
              XLOGF(FATAL, "allocate buffer failed");
            }
            buffers.emplace_back(current);
          }
          if (isWrite) {
            writeIOs.push_back(storageClient->createWriteIO(*chainResult,
                                                            *chunk,
                                                            offset % chunkSize,
                                                            l,
                                                            chunkSize,
                                                            current.ptr(),
                                                            &buffers.back()));
          } else {
            readIOs.push_back(
                storageClient
                    ->createReadIO(*chainResult, *chunk, offset % chunkSize, l, current.ptr(), &buffers.back()));
          }
          current.advance(l);
        }

        if (isWrite) {
          auto writeResult = co_await storageClient->batchWrite(writeIOs, env.userInfo, writeOptions);
          CO_RETURN_AND_LOG_ON_ERROR(writeResult);
        } else {
          auto readResult = co_await storageClient->batchRead(readIOs, env.userInfo, readOptions);
          CO_RETURN_AND_LOG_ON_ERROR(readResult);
        }
        size_t succBytes = 0;
        for (auto &io : readIOs) {
          if (LIKELY(bool(io.result.lengthInfo))) {
            succBytes += *io.result.lengthInfo;
          }
        }
        for (auto &io : writeIOs) {
          if (LIKELY(bool(io.result.lengthInfo))) {
            succBytes += *io.result.lengthInfo;
          }
        }
        readBytes += succBytes;
      }
      co_return Void{};
    }));
  }

  std::atomic<bool> stop = false;
  std::jthread t([&] {
    while (!stop) {
      std::this_thread::sleep_for(1_s);
      XLOGF(WARNING, "{}/s", Size::around(readBytes.exchange(0)));
    }
  });

  auto results = co_await folly::coro::collectAllRange(std::move(total)).scheduleOn(pool.get());
  for (auto result : results) {
    CO_RETURN_AND_LOG_ON_ERROR(result);
  }

  stop = true;
  co_return table;
}

}  // namespace

CoTryTask<void> registerReadBenchHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}

}  // namespace hf3fs::client::cli
