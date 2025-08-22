#pragma once

#include <atomic>
#include <folly/lang/Align.h>
#include <limits>
#include <utility>

#include "common/net/Allocator.h"
#include "common/net/MessageHeader.h"
#include "common/net/RequestOptions.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/utils/DownwardBytes.h"
#include "common/utils/ObjectPool.h"
#include "common/utils/ZSTD.h"

namespace hf3fs::net {

/*
 * A buffer serialized by serde.
 * +----------+-----------------------------------------+
 * |          |             buffer to write             |
 * | headroom +-------------------------------+---------+
 * |          | MessageHeader(checksum, size) | message |
 * +----------+-------------------------------+---------+
 */
class SerdeBuffer {
 public:
  // buffer to write, include message header and content.
  uint8_t *buff() { return self() + headroom_; }
  const uint8_t *buff() const { return self() + headroom_; }

  // message header, include checksum and size of message content.
  auto &header() { return *reinterpret_cast<MessageHeader *>(buff()); }
  auto &header() const { return *reinterpret_cast<const MessageHeader *>(buff()); }

  // message serialized by serde.
  uint8_t *rawMsg() { return buff() + kMessageHeaderSize; }
  const uint8_t *rawMsg() const { return buff() + kMessageHeaderSize; }

  // length of whole message.
  uint32_t length() const { return kMessageHeaderSize + header().size; }
  // length of whole buffer.
  size_t capacity() const { return capacity_; }

  template <class Allocator>
  struct Deleter {
    void operator()(SerdeBuffer *buf) { Allocator::deallocate(buf->self(), buf->capacity()); }
  };

  template <serde::SerdeType T, class Allocator = net::Allocator<4_KB, 1024, 64 * 1024>>
  static auto create(const T &packet, const CoreRequestOptions &options) {
    serde::Out<DownwardBytes<Allocator>> out;
    serde::serialize(packet, out);
    MessageHeader header;
    header.size = out.bytes().size();
    out.bytes().append(&header, sizeof(header));
    out.bytes().reserve(header.size + kMessageHeaderSize + sizeof(SerdeBuffer));  // for headroom.

    uint32_t offset;
    uint32_t capacity;
    std::unique_ptr<SerdeBuffer, Deleter<Allocator>> ptr{
        reinterpret_cast<SerdeBuffer *>(out.bytes().release(offset, capacity))};
    ptr->headroom_ = offset;  // size of headroom.
    ptr->capacity_ = capacity;

    if (packet.timestamp.has_value()) {
      auto timestamp = reinterpret_cast<hf3fs::serde::Timestamp *>(ptr->rawMsg() + ptr->header().size -
                                                                   sizeof(hf3fs::serde::Timestamp));
      if (packet.isRequest()) {
        timestamp->clientSerialized = UtcClock::now().time_since_epoch().count();
      } else {
        timestamp->serverSerialized = UtcClock::now().time_since_epoch().count();
      }
    }

    auto compress = options.compression.enable(capacity);
    if (compress) {
      auto originalSize = ptr->header().size;
      auto compressedOffset = sizeof(SerdeBuffer) + sizeof(MessageHeader);
      auto compressedBound = ZSTD_compressBound(originalSize);
      auto compressedCapacity = compressedOffset + compressedBound;
      std::unique_ptr<SerdeBuffer, Deleter<Allocator>> compressed{
          reinterpret_cast<SerdeBuffer *>(Allocator::allocate(compressedCapacity))};
      compressed->headroom_ = sizeof(SerdeBuffer);
      compressed->capacity_ = compressedCapacity;
      int ret = ZSTD_compress(compressed->rawMsg(),
                              compressedBound,
                              ptr->rawMsg(),
                              ptr->header().size,
                              options.compression.level);
      if (LIKELY(!ZSTD_isError(ret))) {
        compressed->header().size = ret;
        ptr = std::move(compressed);
      } else {
        compress = false;
      }
    }

    ptr->header().checksum = Checksum::calcSerde(ptr->rawMsg(), ptr->header().size, compress);
    return ptr;
  }

 protected:
  SerdeBuffer() = default;
  uint8_t *self() { return reinterpret_cast<uint8_t *>(this); }
  const uint8_t *self() const { return reinterpret_cast<const uint8_t *>(this); }

 private:
  uint32_t headroom_;
  uint32_t capacity_;
};
static_assert(sizeof(SerdeBuffer) == 8);
static_assert(std::is_trivial_v<SerdeBuffer>, "SerdeBuffer is not trivial");

// An item containing a buffer to be written.
struct WriteItem {
  using Pool = ObjectPool<WriteItem, 1024, 64 * 1024>;

  std::atomic<WriteItem *> next = nullptr;
  decltype(SerdeBuffer::create(serde::MessagePacket<>{}, {})) buf;

  uint32_t retryTimes = 0;
  uint32_t maxRetryTimes = kDefaultMaxRetryTimes;

  size_t uuid = std::numeric_limits<size_t>::max();
  bool isReq() const { return uuid != std::numeric_limits<size_t>::max(); }

  template <serde::SerdeType T>
  static auto createMessage(const T &packet, const CoreRequestOptions &options) {
    auto item = Pool::get();
    item->buf = SerdeBuffer::create(packet, options);
    item->maxRetryTimes = options.sendRetryTimes;
    return item;
  }
};
using WriteItemPtr = WriteItem::Pool::Ptr;

class Transport;

class WriteList {
 public:
  WriteList() = default;
  explicit WriteList(WriteItemPtr item) { head_ = tail_ = item.release(); }
  WriteList(WriteItem *head, WriteItem *tail)
      : head_(head),
        tail_(tail) {}
  WriteList(const WriteList &) = delete;
  WriteList(WriteList &&o)
      : head_(std::exchange(o.head_, nullptr)),
        tail_(std::exchange(o.tail_, nullptr)) {}
  ~WriteList() { clear(); }

  bool empty() const { return head_ == nullptr; }

  void clear();

  WriteList extractForRetry();

  void concat(WriteList &&o) {
    if (o.empty()) {
      return;
    }
    if (empty()) {
      head_ = std::exchange(o.head_, nullptr);
      tail_ = std::exchange(o.tail_, nullptr);
    } else {
      tail_->next.store(std::exchange(o.head_, nullptr), std::memory_order_relaxed);
      tail_ = std::exchange(o.tail_, nullptr);
    }
  }

  void setTransport(std::shared_ptr<Transport> tr);

 protected:
  friend class MPSCWriteList;
  WriteItem *head_ = nullptr;
  WriteItem *tail_ = nullptr;
};

class WriteListWithProgress : public WriteList {
 public:
  using WriteList::WriteList;

  uint32_t toIOVec(struct iovec *iovec, uint32_t len, size_t &size);
  void advance(size_t written);

 private:
  // the write offset of the first write item.
  uint32_t firstOffset_ = 0;
};

class MPSCWriteList {
 public:
  ~MPSCWriteList() { takeOut().clear(); }

  // add a list of items. [thread-safe]
  void add(WriteList list);

  // fetch a batch of write items.
  WriteList takeOut();

 private:
  alignas(folly::hardware_destructive_interference_size) std::atomic<WriteItem *> head_;
};

}  // namespace hf3fs::net
