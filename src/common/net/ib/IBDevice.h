#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/IPAddressV4.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/detail/IPAddressSource.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest_prod.h>
#include <infiniband/verbs.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/net/EventLoop.h"
#include "common/net/IfAddrs.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"

namespace hf3fs::net {

class TestIBNotInitialized;
class TestIBDevice;

class IBConfig : public ConfigBase<IBConfig> {
 public:
  static constexpr std::string_view kUnknownZone = "UNKNOWN";
  class Network : public folly::CIDRNetworkV4 {
   public:
    static Result<Network> from(std::string_view str);
    Network(folly::CIDRNetworkV4 network)
        : folly::CIDRNetworkV4(network) {}
    folly::IPAddressV4 ip() const { return first; }
    uint8_t mask() const { return second; }
    std::string toString() const { return fmt::format("{}/{}", ip().str(), mask()); }
  };

  class Subnet : public ConfigBase<Subnet> {
    CONFIG_ITEM(subnet, Network({}));
    CONFIG_ITEM(network_zones, std::vector<std::string>{std::string(kUnknownZone)}, [](auto &v) { return !v.empty(); });
  };

  CONFIG_ITEM(allow_no_usable_devices, false);
  CONFIG_ITEM(skip_inactive_ports, true);
  CONFIG_ITEM(skip_unusable_device, true);

  CONFIG_ITEM(allow_unknown_zone, true);
  CONFIG_ITEM(device_filter, std::vector<std::string>());
  CONFIG_ITEM(default_network_zone, std::string(kUnknownZone), [](auto &v) { return !v.empty(); });
  CONFIG_ITEM(subnets, std::vector<Subnet>());
  CONFIG_ITEM(fork_safe, true);
  CONFIG_ITEM(default_pkey_index, uint16_t(0));
  CONFIG_ITEM(default_roce_pkey_index, uint16_t(0));  // for RoCE, should just use default value 0
  // CONFIG_ITEM(default_gid_index, uint8_t(0));      // for RoCE
  CONFIG_ITEM(default_traffic_class, uint8_t(0));  // for RoCE
  CONFIG_ITEM(prefer_ibdevice, true);              // prefer use ibdevice instead of RoCE device.
};

class IBPort;
class IBManager;

class IBDevice : public std::enable_shared_from_this<IBDevice> {
 public:
  using Ptr = std::shared_ptr<IBDevice>;
  using Config = IBConfig;
  using IB2NetMap = std::map<std::pair<std::string, uint8_t>, std::string>;

  struct Port {
    std::vector<IfAddrs::Addr> addrs;
    std::set<std::string> zones;
    mutable folly::Synchronized<ibv_port_attr> attr;
  };

  struct Deleter {
    void operator()(ibv_pd *pd) const {
      auto ret = ibv_dealloc_pd(pd);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_dealloc_pd failed {}", ret);
    }
    void operator()(ibv_context *context) const {
      auto ret = ibv_close_device(context);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_close_device failed {}", ret);
    }
    void operator()(ibv_cq *cq) const {
      auto ret = ibv_destroy_cq(cq);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_cq failed {}", ret);
    }
    void operator()(ibv_qp *qp) const {
      auto ret = ibv_destroy_qp(qp);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_qp failed {}", ret);
    }
    void operator()(ibv_comp_channel *channel) const {
      auto ret = ibv_destroy_comp_channel(channel);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_comp_channel failed {}", ret);
    }
  };

  static constexpr size_t kMaxDeviceCnt = 4;
  static const std::vector<IBDevice::Ptr> &all();
  static IBDevice::Ptr get(uint8_t devId) {
    if (all().size() < devId) {
      return nullptr;
    }
    return all().at(devId);
  }

  uint8_t id() const { return devId_; }
  const std::string &name() const { return name_; }
  const ibv_device_attr &attr() const { return attr_; }
  ibv_context *context() const { return context_.get(); }
  ibv_pd *pd() const { return pd_.get(); }
  const std::map<uint8_t, Port> &ports() const { return ports_; }
  Result<IBPort> openPort(size_t portNum) const;

  ibv_mr *regMemory(void *addr, size_t length, int access) const;
  int deregMemory(ibv_mr *mr) const;

 private:
  friend class IBManager;
  class BackgroundRunner;
  class AsyncEventHandler;

  static Result<std::vector<IBDevice::Ptr>> openAll(const IBConfig &config);
  static Result<IBDevice::Ptr> open(ibv_device *dev,
                                    uint8_t devId,
                                    std::map<std::pair<std::string, uint8_t>, std::string> ib2net,
                                    std::multimap<std::string, IfAddrs::Addr> ifaddrs,
                                    const IBConfig &config);

  FRIEND_TEST(TestIBDevice, IBConfig);
  static std::vector<IfAddrs::Addr> getIBPortAddrs(const IB2NetMap &ibdev2netdev,
                                                   const IfAddrs::Map &ifaddrs,
                                                   const std::string &devName,
                                                   uint8_t portNum);
  static std::set<std::string> getZonesByAddrs(std::vector<IfAddrs::Addr> addrs,
                                               const IBConfig &config,
                                               const std::string &devName,
                                               uint8_t portNum);

  void checkAsyncEvent() const;
  Result<Void> updatePort(size_t portNum) const;

  uint8_t devId_ = 0;
  std::string name_;
  std::unique_ptr<ibv_context, Deleter> context_;
  std::unique_ptr<ibv_pd, Deleter> pd_;
  ibv_device_attr attr_;
  std::map<uint8_t, Port> ports_;
};

class IBPort {
 public:
  IBPort(std::shared_ptr<const IBDevice> dev = {},
         uint8_t portNum = 0,
         ibv_port_attr attr = {},
         std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid = {});

  uint8_t portNum() const { return portNum_; }
  const IBDevice *dev() const { return dev_.get(); }
  const ibv_port_attr attr() const { return attr_; }
  const std::vector<IfAddrs::Addr> &addrs() const { return dev_->ports().at(portNum_).addrs; }
  const std::set<std::string> &zones() const { return dev_->ports().at(portNum_).zones; }

  bool isRoCE() const { return attr().link_layer == IBV_LINK_LAYER_ETHERNET; }
  bool isInfiniband() const { return attr().link_layer == IBV_LINK_LAYER_INFINIBAND; }
  bool isActive() const { return attr().state == IBV_PORT_ACTIVE || attr().state == IBV_PORT_ACTIVE_DEFER; }
  Result<ibv_gid> queryGid(uint8_t index) const;
  std::pair<ibv_gid, uint8_t> getRoCEv2Gid() const {
    XLOGF_IF(FATAL, !isRoCE(), "port is not RoCE");
    return *rocev2Gid_;
  }

  operator bool() const { return dev_ && portNum_ != 0; }

 private:
  std::shared_ptr<const IBDevice> dev_;
  uint8_t portNum_;
  ibv_port_attr attr_;
  std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid_;
};

class IBSocket;
class IBSocketManager;

class IBManager {
 public:
  static IBManager &instance() {
    static IBManager instance;
    return instance;
  }
  static Result<Void> start(IBConfig config) { return instance().startImpl(config); }
  static void stop() { return instance().reset(); }
  static void close(std::unique_ptr<IBSocket> socket);
  static bool initialized() { return instance().inited_; }

  ~IBManager();

  const std::vector<IBDevice::Ptr> &allDevices() const { return devices_; }
  const std::multimap<std::string, std::pair<IBDevice::Ptr, uint8_t>> zone2port() const { return zone2port_; }
  const auto &config() const { return config_; }

 private:
  IBManager();

  void reset();

  Result<Void> startImpl(IBConfig config);
  void stopImpl() { return reset(); }
  void closeImpl(std::unique_ptr<IBSocket> socket);

  friend class IBSocketManager;

  IBConfig config_;
  std::atomic<bool> inited_ = false;
  std::vector<IBDevice::Ptr> devices_;
  std::multimap<std::string, std::pair<IBDevice::Ptr, uint8_t>> zone2port_;
  std::shared_ptr<EventLoop> eventLoop_;
  std::shared_ptr<IBSocketManager> socketManager_;
  std::shared_ptr<IBDevice::BackgroundRunner> devBgRunner_;
  std::vector<std::shared_ptr<IBDevice::AsyncEventHandler>> devEventHandlers_;
};
}  // namespace hf3fs::net

FMT_BEGIN_NAMESPACE

inline std::string_view ibvLinklayerName(int linklayer) {
  switch (linklayer) {
    case IBV_LINK_LAYER_INFINIBAND:
      return "INFINIBAND";
    case IBV_LINK_LAYER_ETHERNET:
      return "ETHERNET";
    default:
      return "UNSPECIFIED";
  }
}

inline std::string_view ibvPortStateName(ibv_port_state state) {
  switch (state) {
    case IBV_PORT_NOP:
      return "NOP";
    case IBV_PORT_DOWN:
      return "DOWN";
    case IBV_PORT_INIT:
      return "INIT";
    case IBV_PORT_ARMED:
      return "ARMED";
    case IBV_PORT_ACTIVE:
      return "ACTIVE";
    case IBV_PORT_ACTIVE_DEFER:
      return "ACTIVE_DEFER";
    default:
      return "UNKNOWN";
  }
}

template <>
struct formatter<ibv_gid> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const ibv_gid &gid, FormatContext &ctx) const {
    std::string str = fmt::format("{:x}", fmt::join(&gid.raw[0], &gid.raw[sizeof(gid.raw)], ":"));
    return formatter<std::string_view>::format(str, ctx);
  }
};

template <>
struct formatter<ibv_port_attr> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const ibv_port_attr &attr, FormatContext &ctx) const {
    std::string str = fmt::format("{{linklayer: {}, state: {}, lid: {}, mtu: {}}}",
                                  ibvLinklayerName(attr.link_layer),
                                  ibvPortStateName(attr.state),
                                  attr.lid,
                                  magic_enum::enum_name(attr.active_mtu));
    return formatter<std::string_view>::format(str, ctx);
  }
};

template <>
struct formatter<hf3fs::net::IBPort> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::IBPort &port, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(),
                          "{{{}:{}, ifaddrs [{}], zones [{}], {}, {}}}",
                          port.dev()->name(),
                          port.portNum(),
                          fmt::join(port.addrs().begin(), port.addrs().end(), ";"),
                          fmt::join(port.zones().begin(), port.zones().end(), ";"),
                          ibvLinklayerName(port.attr().link_layer),
                          ibvPortStateName(port.attr().state));
  }
};

template <>
struct formatter<hf3fs::net::IBDevice::Config::Network> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::IBDevice::Config::Network &network, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}/{}", network.ip().str(), network.mask());
  }
};

FMT_END_NAMESPACE