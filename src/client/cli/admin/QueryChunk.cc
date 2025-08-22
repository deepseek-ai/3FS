#include "QueryChunk.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/serde/Serde.h"
#include "common/utils/RapidCsv.h"
#include "common/utils/Result.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("query-chunk");
  parser.add_argument("-c", "--chain-id").scan<'u', flat::ChainId::UnderlyingType>();
  parser.add_argument("--chunk").default_value(std::string{});
  parser.add_argument("--read").default_value(false).implicit_value(true);
  parser.add_argument("--index").default_value(uint32_t{0}).scan<'u', uint32_t>();
  parser.add_argument("--touch").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleQueryChunk(IEnv &ienv,
                                                    const argparse::ArgumentParser &parser,
                                                    const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto chunkResult = storage::ChunkId::fromString(parser.get<std::string>("--chunk"));
  CO_RETURN_AND_LOG_ON_ERROR(chunkResult);
  auto chunk = *chunkResult;

  storage::QueryChunkReq req;
  req.chunkId = chunk;
  auto chainIdResult = parser.present<flat::ChainId::UnderlyingType>("-c");
  if (chainIdResult.has_value()) {
    req.chainId = flat::ChainId{chainIdResult.value()};
  } else {
    CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo()->raw();
    if (routingInfo == nullptr) {
      co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
    }

    auto metaChunk = meta::ChunkId::unpack(chunk.data());
    auto statResult =
        co_await env.metaClientGetter()->stat(env.userInfo, meta::InodeId(metaChunk.inode()), std::nullopt, false);
    CO_RETURN_AND_LOG_ON_ERROR(statResult);
    meta::Inode &inode = *statResult;
    std::cout << fmt::format("stat: {}", serde::toJsonString(inode, false, true)) << std::endl;
    if (!inode.isFile()) {
      co_return makeError(StatusCode::kInvalidArg, "not a file");
    }

    const auto &file = inode.asFile();
    auto offset = metaChunk.chunk() * file.layout.chunkSize;
    auto chainResult = file.getChainId(inode, offset, *routingInfo, 0);
    CO_RETURN_AND_LOG_ON_ERROR(chainResult);
    req.chainId = *chainResult;
  }

  auto client = env.storageClientGetter();
  auto res = co_await client->queryChunk(req);
  CO_RETURN_AND_LOG_ON_ERROR(res);
  std::cout << serde::toJsonString(*res, false, true) << std::endl;

  auto chunkSize = 512_KB;
  for (auto &info : *res) {
    if (info.hasValue()) {
      chunkSize = info->meta->innerFileId.chunkSize;
    }
  }

  auto doRead = parser.get<bool>("--read");
  if (doRead) {
    auto index = parser.get<uint32_t>("--index");
    auto rdmabufPool = net::RDMABufPool::create(64_MB, 4);
    auto buffer = co_await rdmabufPool->allocate();
    if (UNLIKELY(!buffer)) {
      XLOGF(ERR, "allocate buffer failed");
      co_return makeError(RPCCode::kRDMANoBuf);
    }
    auto readBuffer = storage::client::IOBuffer{buffer};
    std::vector<storage::client::ReadIO> batch;
    storage::client::ReadOptions readOptions;
    readOptions.set_enableChecksum(true);
    readOptions.targetSelection().set_mode(storage::client::TargetSelectionMode::ManualMode);
    readOptions.targetSelection().set_targetIndex(index);
    auto readIO = client->createReadIO(req.chainId, req.chunkId, 0, chunkSize, (uint8_t *)buffer.ptr(), &readBuffer);
    batch.push_back(std::move(readIO));
    auto result = co_await client->batchRead(batch, env.userInfo, readOptions);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    fmt::println("result: {}", batch.front().result);
  }

  auto doTouch = parser.get<bool>("--touch");
  if (doTouch) {
    auto rdmabufPool = net::RDMABufPool::create(64_MB, 4);
    auto buffer = co_await rdmabufPool->allocate();
    if (UNLIKELY(!buffer)) {
      XLOGF(ERR, "allocate buffer failed");
      co_return makeError(RPCCode::kRDMANoBuf);
    }
    auto writeBuffer = storage::client::IOBuffer{buffer};
    std::vector<storage::client::WriteIO> batch;
    storage::client::WriteOptions writeOptions;
    auto writeIO =
        client->createWriteIO(req.chainId, req.chunkId, 0, 0, chunkSize, (uint8_t *)buffer.ptr(), &writeBuffer);
    batch.push_back(std::move(writeIO));
    auto result = co_await client->batchWrite(batch, env.userInfo, writeOptions);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    fmt::println("result: {}", batch.front().result);
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerQueryChunkHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleQueryChunk);
}

}  // namespace hf3fs::client::cli
