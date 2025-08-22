#pragma once

#include <bit>
#include <cstdint>
#include <folly/hash/Checksum.h>
#include <type_traits>

#include "common/net/Network.h"
#include "common/utils/Size.h"

namespace hf3fs::net {

constexpr uint32_t kMessageHeaderSize = 8u;
constexpr uint8_t kSerdeMessageMagicNum = 0x86;
// The most extreme case of storage sync:
// target size 30TB / chunk size 512KB * single meta size 37B / compression ratio 5 = 444MB
constexpr size_t kMessageMaxSize = 512_MB;
constexpr size_t kMessageReadBufferSize = 32_KB;

struct MessageHeader {
  uint32_t checksum;
  uint32_t size;

  bool isSerdeMessage() const { return (checksum & 0xfeu) == kSerdeMessageMagicNum; }
  bool isCompressed() const { return isCompressed(checksum); }
  static bool isCompressed(uint32_t checksum) { return checksum & 1; }
};
static_assert(sizeof(MessageHeader) == kMessageHeaderSize, "sizeof(MessageHeader) != kMessageHeaderSize");
static_assert(kMessageHeaderSize % sizeof(uint64_t) == 0, "MessageHeader is not aligned by sizeof(uint64_t)!");
static_assert(std::is_trivial_v<MessageHeader>, "std::is_trivial_v<MessageHeader> is false!");

struct Checksum {
  // calculate crc32c and overwrite the lower 8 bits with serde magic number.
  static inline uint32_t calcSerde(const uint8_t *data, size_t size, bool compressed = false) {
    uint32_t crc32c = folly::crc32c(data, size, 0);
    return (crc32c & ~0xffu) | kSerdeMessageMagicNum | compressed;
  }
};

class MessageWrapper : folly::MoveOnly {
 public:
  MessageWrapper(IOBufPtr buf)
      : buf_(std::move(buf)) {}

  // get the capacity of the total buffer.
  size_t capacity() const { return buf_->capacity(); }

  // get the length of the remaining buffer.
  size_t length() const { return buf_->length(); }

  // check whether the current header is complete.
  bool headerComplete() const { return kMessageHeaderSize <= length(); }

  // get the header of current message.
  auto &header() const { return *reinterpret_cast<const MessageHeader *>(buf_->data()); }

  // get the length of the current message.
  size_t messageLength() const { return kMessageHeaderSize + header().size; }

  // check whether the current message is complete.
  bool messageComplete() const { return messageLength() <= length(); }

  // clone current message.
  IOBufPtr cloneMessage() {
    auto newBuf = buf_->cloneOne();
    newBuf->trimEnd(length() - messageLength());
    newBuf->trimStart(sizeof(MessageHeader));
    return newBuf;
  }

  // seek to next message header.
  void next() { buf_->trimStart(messageLength()); }

  // allocate a empty buffer.
  static IOBufPtr allocate(size_t size);

  // create a new buffer with the remaining data.
  IOBufPtr createFromRemain(size_t minSize);

  // check whether at least one message is complete in this wrapper.
  bool hasMessages() const { return buf_->headroom(); }

  // seek to begin position.
  void seekBegin() { buf_->prepend(buf_->headroom()); }

  // detach the buffer.
  IOBufPtr detach() { return std::move(buf_); }

  // is serde message.
  bool isSerdeMessage() const { return header().isSerdeMessage(); }

 private:
  IOBufPtr buf_;
};

}  // namespace hf3fs::net
