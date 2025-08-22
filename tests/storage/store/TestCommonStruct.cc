#include "common/serde/Serde.h"
#include "common/utils/Reflection.h"
#include "fbs/storage/Common.h"
#include "fbs/storage/Service.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage::test {
namespace {

TEST(TestCommonStruct, Normal) {
  {
    ChunkId ser(0xface, 0xbeef);
    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 8 + 8);

    ChunkId des;
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_EQ(des, ser);

    auto str = ser.describe();
    auto result = ChunkId::fromString(str);
    ASSERT_OK(result);
    ASSERT_EQ(*result, ser);

    auto next = ChunkId{0xface, 0xbef0};
    ASSERT_EQ(ser.nextChunkId(), next);
  }

  {
    auto id = ChunkId{0x0000, 0x007f};
    auto next = ChunkId{0x0000, 0x0080};
    ASSERT_EQ(id.nextChunkId(), next);
  }
  {
    auto id = ChunkId{0x00ff, 0xffffffffffffffffull};
    auto next = ChunkId{0x0100, 0x0000000000000000ull};
    ASSERT_EQ(id.nextChunkId(), next);
  }

  {
    std::string str = "00000000-00000346-85270000-0000000B";
    auto chunk = ChunkId::fromString(str);
    ASSERT_EQ(chunk->describe(), str);
  }

  {
    ChecksumInfo ser;
    ser.type = ChecksumType::CRC32;
    ser.value = 0xff;
    auto out = serde::serialize(ser);
    ASSERT_EQ(out.size(), 1 + 1 + 4);

    ChecksumInfo des;
    ASSERT_OK(serde::deserialize(des, out));
    ASSERT_EQ(des, ser);
  }

  {
    BatchReadReq req;
    req.payloads.emplace_back();
    req.payloads.back().key.chunkId = ChunkId(1, 1);
    req.payloads.back().rdmabuf.rkeys()[0].devId = 0;
    auto out = serde::serialize(req);
    XLOGF(INFO, "single read req size: {}, json: {}", out.length(), req);

    auto front = req.payloads.front();
    req.payloads.resize(16, front);
    out = serde::serialize(req);
    XLOGF(INFO, "10 read req size: {}", out.length());
  }

  refl::Helper::iterate<StorageSerde<>>([](auto type) {
    using T = decltype(type);
    XLOGF(INFO,
          "method: {}, ID: {}\n  Req: {}\n  Rsp: {}",
          T::name,
          T::id,
          serde::toJsonString(typename T::ReqType{}),
          serde::toJsonString(typename T::RspType{}));
  });
}

}  // namespace
}  // namespace hf3fs::storage::test
