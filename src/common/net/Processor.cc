#include "common/net/Processor.h"

#include "common/monitor/Recorder.h"

namespace hf3fs::net {
namespace {

monitor::CountRecorder compressedCount{"common.rpc.compressed_count"};
monitor::CountRecorder compressedBytes{"common.rpc.compressed_bytes"};
monitor::CountRecorder decompressedBytes{"common.rpc.decompressed_bytes"};

}  // namespace

Result<Void> Processor::decompressSerdeMsg(IOBufPtr &buf, TransportPtr &tr) {
  auto length = buf->length();
  auto bound = ZSTD_decompressBound(buf->data(), length);
  if (UNLIKELY(ZSTD_isError(bound))) {
    XLOGF(ERR, "decompress get bound error: {}, peer: {}", ZSTD_getErrorName(bound), tr->describe());
    tr->invalidate();
    return makeError(RPCCode::kVerifyRequestFailed);
  }
  auto newBuf = IOBuf::createCombined(bound);
  auto ret = ZSTD_decompress(newBuf->writableBuffer(), bound, buf->data(), length);
  if (UNLIKELY(ZSTD_isError(ret))) {
    XLOGF(ERR, "decompress message error: {}, peer: {}", ZSTD_getErrorName(ret), tr->describe());
    tr->invalidate();
    return makeError(RPCCode::kVerifyRequestFailed);
  }
  newBuf->append(ret);
  buf = std::move(newBuf);
  compressedCount.addSample(1);
  compressedBytes.addSample(length);
  decompressedBytes.addSample(ret);
  return Void{};
}

}  // namespace hf3fs::net
