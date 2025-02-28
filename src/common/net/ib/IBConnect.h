#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <folly/Overload.h>
#include <folly/container/BitIterator.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/lang/Bits.h>
#include <functional>
#include <infiniband/verbs.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/net/ib/IBDevice.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

struct IBDeviceInfo {
 public:
  struct PortInfo {
    SERDE_STRUCT_FIELD(port, uint8_t(0));
    SERDE_STRUCT_FIELD(zones, std::vector<std::string>());
    SERDE_STRUCT_FIELD(link_layer, uint8_t(IBV_LINK_LAYER_UNSPECIFIED));
  };

  SERDE_STRUCT_FIELD(dev, uint8_t(0));
  SERDE_STRUCT_FIELD(dev_name, std::string());
  SERDE_STRUCT_FIELD(ports, std::vector<PortInfo>());
};

struct IBQueryReq {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct IBQueryRsp {
  SERDE_STRUCT_FIELD(devices, std::vector<IBDeviceInfo>());

 public:
  struct Match {
    uint8_t remoteDev;
    std::string remoteDevName;
    uint8_t remotePort;
    IBDevice::Ptr localDev;
    uint8_t localPort;
    std::string zone;

    std::string describe() const {
      if (zone.empty()) {
        return fmt::format("{{local {}:{} remote {}:{}}}", localDev->name(), localPort, remoteDevName, remotePort);
      } else {
        return fmt::format("{{local {}:{}, remote {}:{}, zone {}}}",
                           localDev->name(),
                           localPort,
                           remoteDevName,
                           remotePort,
                           zone);
      }
    }
  };
  Result<Match> selectDevice(const std::string &describe, uint64_t counter);
  void findMatchDevices(const std::string &describe,
                        std::vector<IBQueryRsp::Match> &ibMatches,
                        std::vector<IBQueryRsp::Match> &roceMatches,
                        bool checkZone);
};

struct IBConnectConfig {
  SERDE_STRUCT_FIELD(sl, uint8_t(0));
  SERDE_STRUCT_FIELD(traffic_class, uint8_t(0));
  SERDE_STRUCT_FIELD(gid_index, uint8_t(0));  // unused
  SERDE_STRUCT_FIELD(pkey_index, uint16_t(0));

  SERDE_STRUCT_FIELD(start_psn, uint32_t(0));
  SERDE_STRUCT_FIELD(min_rnr_timer, uint8_t(0));
  SERDE_STRUCT_FIELD(timeout, uint8_t(0));
  SERDE_STRUCT_FIELD(retry_cnt, uint8_t(0));
  SERDE_STRUCT_FIELD(rnr_retry, uint8_t(0));

  SERDE_STRUCT_FIELD(max_sge, uint32_t(0));
  SERDE_STRUCT_FIELD(max_rdma_wr, uint32_t(0));
  SERDE_STRUCT_FIELD(max_rdma_wr_per_post, uint32_t(0));
  SERDE_STRUCT_FIELD(max_rd_atomic, uint32_t(0));

  SERDE_STRUCT_FIELD(buf_size, uint32_t(0));
  SERDE_STRUCT_FIELD(send_buf_cnt, uint32_t(0));
  SERDE_STRUCT_FIELD(buf_ack_batch, uint32_t(0));
  SERDE_STRUCT_FIELD(buf_signal_batch, uint32_t(0));
  SERDE_STRUCT_FIELD(event_ack_batch, uint32_t(0));

  SERDE_STRUCT_FIELD(record_latency_per_peer, false);

 public:
  uint32_t qpAckBufs() const { return (send_buf_cnt + buf_ack_batch - 1) / buf_ack_batch + 4; }
  uint32_t qpMaxSendWR() const {
    return send_buf_cnt + qpAckBufs() + max_rdma_wr + 1 /* slot to send close msg */ + 1 /* slot to send check msg. */;
  }
  uint32_t qpMaxRecvWR() const { return send_buf_cnt + qpAckBufs() + 1 /* slot to recv close msg */; }
  uint32_t qpMaxCQEntries() const { return qpMaxSendWR() + qpMaxRecvWR(); }
};

struct IBConnectIBInfo {
  SERDE_STRUCT_FIELD(lid, uint16_t(0));

 public:
  static constexpr auto kType = IBV_LINK_LAYER_INFINIBAND;
};

struct IBConnectRoCEInfo {
  SERDE_STRUCT_FIELD(gid, ibv_gid{});

 public:
  static constexpr auto kType = IBV_LINK_LAYER_ETHERNET;
};

struct IBConnectInfo {
  SERDE_STRUCT_FIELD(hostname, std::string());
  SERDE_STRUCT_FIELD(dev, uint8_t(0));
  SERDE_STRUCT_FIELD(dev_name, std::string());
  SERDE_STRUCT_FIELD(port, uint8_t(0));

  SERDE_STRUCT_FIELD(mtu, uint8_t(0));
  SERDE_STRUCT_FIELD(qp_num, uint32_t(0));
  SERDE_STRUCT_FIELD(linklayer, (std::variant<IBConnectIBInfo, IBConnectRoCEInfo>()));

 public:
  auto getLinkerLayerType() const {
    return folly::variant_match(linklayer, [](auto &v) { return std::decay_t<decltype(v)>::kType; });
  }
};

struct IBConnectReq : IBConnectInfo {
  SERDE_STRUCT_FIELD(target_dev, uint8_t(0));
  SERDE_STRUCT_FIELD(target_devname, std::string());
  SERDE_STRUCT_FIELD(target_port, uint8_t(0));
  SERDE_STRUCT_FIELD(config, IBConnectConfig());

 public:
  IBConnectReq()
      : IBConnectReq({}, 0, "", 0, {}) {}
  IBConnectReq(IBConnectInfo info,
               uint8_t target_dev,
               std::string target_devname,
               uint8_t target_port,
               IBConnectConfig config)
      : IBConnectInfo(info),
        target_dev(target_dev),
        target_devname(std::move(target_devname)),
        target_port(target_port),
        config(config) {}
};
static_assert(serde::SerializableToBytes<IBConnectReq> && serde::SerializableToJson<IBConnectReq>);

struct IBConnectRsp : IBConnectInfo {
  SERDE_STRUCT_FIELD(dummy, Void{});

 public:
  IBConnectRsp(IBConnectInfo info = {})
      : IBConnectInfo(info) {}
};
static_assert(serde::SerializableToBytes<IBConnectRsp> && serde::SerializableToJson<IBConnectRsp>);

}  // namespace hf3fs::net

template <>
struct hf3fs::serde::SerdeMethod<ibv_gid> {
  static std::string_view serdeTo(const ibv_gid &gid) {
    return std::string_view((const char *)&gid.raw[0], sizeof(ibv_gid::raw));
  }
  static Result<ibv_gid> serdeFrom(std::string_view s) {
    if (s.length() != sizeof(ibv_gid::raw)) {
      return makeError(StatusCode::kDataCorruption);
    } else {
      ibv_gid gid;
      memcpy(gid.raw, s.data(), s.length());
      return gid;
    }
  }
};
