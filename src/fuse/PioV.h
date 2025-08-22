#pragma once

#include <functional>

#include "client/meta/MetaClient.h"
#include "client/storage/StorageClient.h"
#include "common/utils/Result.h"

namespace hf3fs::lib::agent {
using flat::UserInfo;
class PioV {
 public:
  PioV(storage::client::StorageClient &storageClient, int chunkSizeLim, std::vector<ssize_t> &res);
  hf3fs::Result<Void> addRead(size_t idx,
                              const meta::Inode &inode,
                              uint16_t track,
                              off_t off,
                              size_t len,
                              void *buf,
                              storage::client::IOBuffer &memh);
  // if metaClient and userInfo are not nullptr,
  // meta server will be contacted for latest file length if known length is shorter than off
  CoTryTask<bool> checkWriteOff(size_t idx,
                                meta::client::MetaClient *metaClient,
                                const UserInfo *userInfo,
                                const meta::Inode &inode,
                                size_t off);
  hf3fs::Result<Void> addWrite(size_t idx,
                               const meta::Inode &inode,
                               uint16_t track,
                               off_t off,
                               size_t len,
                               const void *buf,
                               storage::client::IOBuffer &memh);
  CoTryTask<void> executeRead(const UserInfo &userInfo,
                              const storage::client::ReadOptions &options = storage::client::ReadOptions());
  CoTryTask<void> executeWrite(const UserInfo &userInfo,
                               const storage::client::WriteOptions &options = storage::client::WriteOptions());
  void finishIo(bool allowHoles);

 private:
  Result<Void> chunkIo(
      const meta::Inode &inode,
      uint16_t track,
      off_t off,
      size_t len,
      std::function<void(storage::ChainId, storage::ChunkId, uint32_t, uint32_t, uint32_t)> &&consumeChunk);

 private:
  storage::client::StorageClient &storageClient_;
  int chunkSizeLim_;
  std::shared_ptr<flat::RoutingInfo> routingInfo_;
  std::vector<ssize_t> &res_;
  std::vector<storage::client::ReadIO> rios_;
  std::vector<storage::client::WriteIO> wios_;
  std::vector<storage::client::TruncateChunkOp> trops_;
  std::map<meta::InodeId, size_t> potentialLens_;
};
}  // namespace hf3fs::lib::agent
