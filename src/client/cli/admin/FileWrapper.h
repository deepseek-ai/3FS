#pragma once

#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/md5.h>
#include <optional>
#include <sstream>

#include "client/cli/admin/AdminEnv.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "client/storage/TargetSelection.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {

struct Block {
  SERDE_STRUCT_FIELD(chainId, storage::ChainId{});
  SERDE_STRUCT_FIELD(chunkId, storage::ChunkId{});
  SERDE_STRUCT_FIELD(offset, uint64_t{});
  SERDE_STRUCT_FIELD(length, uint64_t{});
};

class FileWrapper {
 public:
  FileWrapper(AdminEnv &env)
      : env_(env) {}

  auto &file() { return inode_.asFile(); }
  auto &file() const { return inode_.asFile(); }
  auto length() const { return file().length; }

  auto truncate(size_t end) { return env_.metaClientGetter()->truncate(env_.userInfo, inode_.id, end); }
  auto sync() { return env_.metaClientGetter()->sync(env_.userInfo, inode_.id, false, true, std::nullopt); }

  Result<std::vector<Block>> prepareBlocks(size_t offset, size_t end, const flat::RoutingInfo &routingInfo);
  Result<uint32_t> replicasNum();
  Result<Void> showChunks(size_t offset, size_t length);

  CoTryTask<storage::ChecksumInfo> readFile(
      std::ostream &out,
      size_t length,
      size_t offset,
      bool checksum = true,
      bool hex = false,
      storage::client::TargetSelectionMode mode = storage::client::TargetSelectionMode::Default,
      MD5_CTX *md5 = nullptr,
      bool fillZero = false,
      bool verbose = false,
      uint32_t targetIndex = 0);

  static CoTryTask<storage::ChecksumInfo> readFile(
      AdminEnv &env,
      std::ostream &out,
      const std::vector<Block> &blocks,
      bool checksum = true,
      bool hex = false,
      storage::client::TargetSelectionMode mode = storage::client::TargetSelectionMode::Default,
      MD5_CTX *md5 = nullptr,
      bool fillZero = false,
      bool verbose = false,
      uint32_t targetIndex = 0);

  CoTryTask<storage::ChecksumInfo> writeFile(std::istream &in,
                                             size_t length,
                                             size_t offset,
                                             Duration timeout,
                                             bool syncAfterWrite = true);

  static CoTryTask<FileWrapper> openOrCreateFile(AdminEnv &env, const Path &src, bool createIsMissing = false);

 private:
  AdminEnv &env_;
  SERDE_CLASS_FIELD(inode, meta::Inode{});
};

}  // namespace hf3fs::client::cli
