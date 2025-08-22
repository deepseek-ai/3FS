#include "PioV.h"

namespace hf3fs::lib::agent {
PioV::PioV(storage::client::StorageClient &storageClient, int chunkSizeLim, std::vector<ssize_t> &res)
    : storageClient_(storageClient),
      chunkSizeLim_(chunkSizeLim),
      res_(res) {
  auto &mgmtdClient = storageClient_.getMgmtdClient();
  auto routingInfo = mgmtdClient.getRoutingInfo();
  XLOGF_IF(DFATAL, !routingInfo || !routingInfo->raw(), "RoutingInfo not found");
  routingInfo_ = routingInfo->raw();
}

hf3fs::Result<Void> PioV::addRead(size_t idx,
                                  const meta::Inode &inode,
                                  uint16_t track,
                                  off_t off,
                                  size_t len,
                                  void *buf,
                                  storage::client::IOBuffer &memh) {
  if (!wios_.empty()) {
    return makeError(StatusCode::kInvalidArg, "adding read to write operations");
  } else if (!inode.isFile()) {
    res_[idx] = -static_cast<ssize_t>(MetaCode::kNotFile);
    return Void{};
  }

  if (rios_.empty()) {
    rios_.reserve(res_.size());
  }

  size_t bufOff = 0;
  RETURN_ON_ERROR(chunkIo(inode,
                          track,
                          off,
                          len,
                          [this, &memh, &bufOff, idx, buf](storage::ChainId chain,
                                                           storage::ChunkId chunk,
                                                           uint32_t,
                                                           uint32_t chunkOff,
                                                           uint32_t chunkLen) {
                            rios_.emplace_back(storageClient_.createReadIO(chain,
                                                                           chunk,
                                                                           chunkOff,
                                                                           chunkLen,
                                                                           (uint8_t *)buf + bufOff,
                                                                           &memh,
                                                                           reinterpret_cast<void *>(idx)));
                            bufOff += chunkLen;
                          }));

  return Void{};
}

hf3fs::Result<Void> PioV::addWrite(size_t idx,
                                   const meta::Inode &inode,
                                   uint16_t track,
                                   off_t off,
                                   size_t len,
                                   const void *buf,
                                   storage::client::IOBuffer &memh) {
  if (!rios_.empty()) {
    return makeError(StatusCode::kInvalidArg, "adding write to read operations");
  } else if (!inode.isFile()) {
    res_[idx] = -static_cast<ssize_t>(MetaCode::kNotFile);
    return Void{};
  }

  if (wios_.empty()) {
    wios_.reserve(res_.size());
  }

  size_t bufOff = 0;
  RETURN_ON_ERROR(chunkIo(inode,
                          track,
                          off,
                          len,
                          [this, &inode, &memh, &bufOff, idx, buf, off](storage::ChainId chain,
                                                                        storage::ChunkId chunk,
                                                                        uint32_t chunkSize,
                                                                        uint32_t chunkOff,
                                                                        uint32_t chunkLen) {
                            wios_.emplace_back(storageClient_.createWriteIO(chain,
                                                                            chunk,
                                                                            chunkOff,
                                                                            chunkLen,
                                                                            chunkSize,
                                                                            (uint8_t *)buf + bufOff,
                                                                            &memh,
                                                                            reinterpret_cast<void *>(idx)));
                            bufOff += chunkLen;
                            potentialLens_[inode.id] = std::max(potentialLens_[inode.id], off + bufOff + chunkLen);
                          }));

  return Void{};
}

Result<Void> PioV::chunkIo(
    const meta::Inode &inode,
    uint16_t track,
    off_t off,
    size_t len,
    std::function<void(storage::ChainId, storage::ChunkId, uint32_t, uint32_t, uint32_t)> &&consumeChunk) {
  const auto &f = inode.asFile();
  auto chunkSize = f.layout.chunkSize;
  auto chunkOff = off % chunkSize;

  auto rcs = chunkSizeLim_ ? std::min((size_t)chunkSizeLim_, chunkSize.u64()) : chunkSize.u64();

  for (size_t lastL = 0, l = std::min((size_t)(chunkSize - chunkOff), len);  // l is within a chunk
       l < len + chunkSize;                                                  // for the last chunk
       lastL = l, l += chunkSize) {
    l = std::min(l, len);  // l is always growing longer
    auto opOff = off + lastL;

    auto chain = f.getChainId(inode, opOff, *routingInfo_, track);
    RETURN_ON_ERROR(chain);
    auto fchunk = f.getChunkId(inode.id, opOff);
    RETURN_ON_ERROR(fchunk);
    auto chunk = storage::ChunkId(*fchunk);
    auto chunkLen = l - lastL;

    for (size_t co = 0; co < chunkLen; co += rcs) {
      consumeChunk(*chain, chunk, chunkSize, chunkOff + co, std::min(rcs, chunkLen - co));
    }

    chunkOff = 0;  // chunks other than first always starts from 0
  }
  return Void{};
}

CoTryTask<void> PioV::executeRead(const UserInfo &userInfo, const storage::client::ReadOptions &options) {
  assert(wios_.empty() && trops_.empty());

  if (rios_.empty()) {
    co_return Void{};
  }

  co_return co_await storageClient_.batchRead(rios_, userInfo, options);
}

CoTryTask<void> PioV::executeWrite(const UserInfo &userInfo, const storage::client::WriteOptions &options) {
  assert(rios_.empty());

  if (wios_.empty()) {
    co_return Void{};
  }

  if (!trops_.empty()) {
    std::vector<storage::client::TruncateChunkOp *> failed;
    std::set<size_t> badWios;
    auto r = co_await storageClient_.truncateChunks(trops_, userInfo, options, &failed);
    CO_RETURN_ON_ERROR(r);
    if (!failed.empty()) {
      for (auto op : failed) {
        res_[reinterpret_cast<size_t>(op->userCtx)] = -static_cast<ssize_t>(op->result.lengthInfo.error().code());
        for (size_t i = 0; i < wios_.size(); ++i) {
          if (wios_[i].userCtx == op->userCtx) {
            badWios.insert(i);
          }
        }
      }
      std::vector<storage::client::WriteIO> wios2;
      wios2.reserve(wios_.size() - badWios.size());
      for (size_t i = 0; i < wios_.size(); ++i) {
        if (badWios.find(i) == badWios.end()) {
          auto &wio = wios_[i];
          wios2.emplace_back(storageClient_.createWriteIO(wio.routingTarget.chainId,
                                                          wio.chunkId,
                                                          wio.offset,
                                                          wio.length,
                                                          wio.chunkSize,
                                                          wio.data,
                                                          wio.buffer,
                                                          wio.userCtx));
        }
      }
      std::swap(wios_, wios2);
    }
  }

  co_return co_await storageClient_.batchWrite(wios_, userInfo, options);
}

template <typename Io>
void concatIoRes(bool read, std::vector<ssize_t> &res, const Io &ios, bool allowHoles) {
  ssize_t lastIovIdx = -1;
  bool inHole = false;
  std::optional<size_t> holeIo = 0;
  size_t holeOff = 0;
  size_t holeSize = 0;
  ssize_t iovIdx = 0;
  for (size_t i = 0; i < ios.size(); ++i, lastIovIdx = iovIdx) {
    const auto &io = ios[i];
    iovIdx = reinterpret_cast<ssize_t>(io.userCtx);
    uint32_t iolen = 0;
    if (io.result.lengthInfo) {
      iolen = *io.result.lengthInfo;
      if (iolen > 0 && inHole && lastIovIdx == iovIdx) {
        // the front part of the data read from a chunk can never be part of a hole when anything is read from the chunk
        // storage server promises that, or how can it tell us that it only reads into the buffer from the middle?
        // so the hole size always ends at the last chunk end, and we can add it to res to calc the correct read size
        // and if the hole is not a hole, but the eof, the prev res will be the no. of bytes read from the file

        const auto &lastIo = ios[i - 1];
        auto lastChunk = meta::ChunkId::unpack(lastIo.chunkId.data());
        auto chunk = meta::ChunkId::unpack(io.chunkId.data());
        XLOGF(ERR,
              "found hole when {}ing inode id {}, hole starts before chunk idx {} chain id {} got {}_B in chunk "
              "idx {} / {} chain id {} after hole iov idx {} last iov idx {} hole io idx {} off in first io {} size {}",
              read ? "read" : "writ",
              lastChunk.inode().u64(),
              lastChunk.chunk(),
              lastIo.routingTarget.chainId,
              iolen,
              chunk.inode().u64(),
              chunk.chunk(),
              io.routingTarget.chainId,
              iovIdx,
              lastIovIdx,
              *holeIo,
              holeOff,
              holeSize);

        if (read && allowHoles) {  // zerofill the hole we found
          auto &hio = ios[*holeIo];
          memset(hio.data + holeOff, 0, hio.length - holeOff);
          for (size_t j = *holeIo + 1; j < i; ++j) {
            memset(ios[j].data, 0, ios[j].length);
          }

          res[iovIdx] += holeSize;

          inHole = false;  // out of hole now, but we may begin a new hole
          holeIo = std::nullopt;
        } else {
          res[iovIdx] = -static_cast<ssize_t>(ClientAgentCode::kHoleInIoOutcome);
        }
      } else if (lastIovIdx != iovIdx) {
        inHole = false;
        holeIo = std::nullopt;
      }
    } else if (read && io.result.lengthInfo.error().code() == StorageClientCode::kChunkNotFound) {
      // ignore
    } else {
      if (res[iovIdx] >= 0) {
        res[iovIdx] = -static_cast<ssize_t>(io.result.lengthInfo.error().code());
      }
    }

    if (res[iovIdx] < 0) {
      continue;
    }

    if (iolen < io.length) {  // shorter than expected
      inHole = true;
      if (!holeIo) {
        holeIo = i;
        holeOff = iolen;
        holeSize = 0;
      }
      holeSize += io.length - iolen;
    }
    res[iovIdx] += iolen;
  }
}

void PioV::finishIo(bool allowHoles) {
  if (wios_.empty()) {
    concatIoRes(true, res_, rios_, allowHoles);
  } else {
    concatIoRes(false, res_, wios_, false);
  }
}
}  // namespace hf3fs::lib::agent
