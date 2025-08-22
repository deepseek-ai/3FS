#include "UserToken.h"

#include <folly/Random.h>
#include <folly/base64.h>
#include <folly/hash/Checksum.h>
#include <folly/lang/Bits.h>

#include "fdb/FDBTransaction.h"

namespace hf3fs::core {
namespace {
const uint16_t magicNum = 0xF1DB;
// clang-format off
constexpr std::array<uint8_t, 18> mapping = {
  12, 6, 0,
  13, 7, 1,
  14, 8, 2,
  15, 9, 3,
  16, 10, 4,
  17, 11, 5,
};
// clang-format on
}  // namespace

String encodeUserToken(uint32_t uid, uint64_t randomv) {
  static_assert(folly::Endian::order == folly::Endian::Order::LITTLE);

  std::array<uint8_t, 18> tmp;
  auto *p = tmp.data();

  static_assert(sizeof(magicNum) == 2);
  std::memcpy(p, &magicNum, sizeof(magicNum));
  p += sizeof(magicNum);

  static_assert(sizeof(uid) == 4);
  std::memcpy(p, &uid, sizeof(uid));
  p += sizeof(uid);

  static_assert(sizeof(randomv) == 8);
  std::memcpy(p, &randomv, sizeof(randomv));
  p += sizeof(randomv);

  auto crc = folly::crc32(tmp.data(), 14, 0);
  static_assert(sizeof(crc) == 4);
  std::memcpy(p, &crc, sizeof(crc));

  std::array<char, 18> buffer;
  for (size_t i = 0; i < 18; ++i) {
    buffer[mapping[i]] = tmp[i];
  }
  return folly::base64Encode(std::string_view(buffer.data(), buffer.size()));
}

Result<std::pair<uint32_t, uint64_t>> decodeUserToken(std::string_view token) {
  try {
    auto decoded = folly::base64Decode(token);
    if (decoded.size() != 18) return makeError(StatusCode::kInvalidFormat, "Decode token fail: invalid format");

    std::array<uint8_t, 18> tmp;
    for (size_t i = 0; i < 18; ++i) {
      tmp[i] = decoded[mapping[i]];
    }

    uint16_t mn = 0;
    uint32_t uid = 0, crc = 0;
    uint64_t timestamp = 0;
    auto *p = tmp.data();
    std::memcpy(&mn, p, sizeof(mn));
    p += sizeof(mn);
    std::memcpy(&uid, p, sizeof(uid));
    p += sizeof(uid);
    std::memcpy(&timestamp, p, sizeof(timestamp));
    p += sizeof(timestamp);
    std::memcpy(&crc, p, sizeof(crc));

    if (mn != magicNum) {
      return makeError(StatusCode::kInvalidFormat, "Decode token fail: invalid format");
    }

    auto expectedCrc = folly::crc32(tmp.data(), 14, 0);
    if (crc != expectedCrc) {
      return makeError(StatusCode::kInvalidFormat, "Decode token fail: invalid crc");
    }
    return std::make_pair(uid, timestamp);
  } catch (const folly::base64_decode_error &e) {
    return makeError(StatusCode::kInvalidFormat, "Decode token fail: {}", e.what());
  }
}

Result<flat::Uid> decodeUidFromUserToken(std::string_view token) {
  auto r = decodeUserToken(token);
  RETURN_ON_ERROR(r);
  auto [uid, ts] = *r;
  return flat::Uid{uid};
}

CoTryTask<String> encodeUserToken(uint32_t uid, kv::IReadOnlyTransaction &txn) {
  co_return encodeUserToken(uid, folly::Random::secureRand64());
}
}  // namespace hf3fs::core
