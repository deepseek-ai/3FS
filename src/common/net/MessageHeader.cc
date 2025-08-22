#include "common/net/MessageHeader.h"

#include "common/net/Allocator.h"

namespace hf3fs::net {
namespace {

using InnerAllocator = net::Allocator<kMessageReadBufferSize, 64, 64 * 64>;  // at most 128MB in global cache.
void freeFunction(void *buf, void *userData) {
  InnerAllocator::deallocate(reinterpret_cast<uint8_t *>(buf), reinterpret_cast<size_t>(userData));
}

}  // namespace

IOBufPtr MessageWrapper::allocate(size_t size) {
  return IOBuf::takeOwnership(InnerAllocator::allocate(size), size, 0, 0, freeFunction, reinterpret_cast<void *>(size));
}

IOBufPtr MessageWrapper::createFromRemain(size_t minSize) {
  if (headerComplete() && minSize < messageLength()) {
    minSize = messageLength();
  }

  auto newBuf = MessageWrapper::allocate(minSize);
  size_t remainSize = length();
  if (remainSize) {
    ::memcpy(newBuf->writableBuffer(), buf_->data(), remainSize);
    newBuf->append(remainSize);
    buf_->trimEnd(remainSize);
  }
  return newBuf;
}

}  // namespace hf3fs::net
