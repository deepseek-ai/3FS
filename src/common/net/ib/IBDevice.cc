#include "IBDevice.h"

#include <algorithm>
#include <asm-generic/errno.h>
#include <bits/types/struct_itimerspec.h>
#include <bits/types/struct_timespec.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <folly/experimental/TimerFD.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <folly/small_vector.h>
#include <folly/system/ThreadName.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/net/EventLoop.h"
#include "common/net/IfAddrs.h"
#include "common/net/ib/IBSocket.h"
#include "common/utils/Duration.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/String.h"

using namespace hf3fs::net;

/* from rdma-core
 * GID types as appear in sysfs, no change is expected as of ABI
 * compatibility.
 */
#define V1_TYPE "IB/RoCE v1"
#define V2_TYPE "RoCE v2"

namespace hf3fs::net {

namespace {
monitor::ValueRecorder deviceCnt("common.ib.devicecnt", {});
monitor::CountRecorder mrcnt("common.ib.mrcnt", {}, false);
monitor::CountRecorder mrSize("common.ib.mrsize", {}, false);
monitor::CountRecorder mrError("common.ib.mrerr");
monitor::LatencyRecorder mrLatency("common.ib.mr_reg_latency");
monitor::CountRecorder events("common.ib.events");

monitor::TagSet getPortTags(const IBDevice &device, uint8_t portNum, const std::set<std::string> &ports) {
  monitor::TagSet tag;
  tag.addTag("instance", fmt::format("{}:{}", device.name(), portNum));
  tag.addTag("tag", fmt::format("{}", fmt::join(ports.begin(), ports.end(), ";")));
  return tag;
}

// Ubuntu 20.04's rdma-core doesn't have ibv_query_gid_ex, and ibv_query_gid_type is a private symbol, so we have to
// implement this.
Result<std::pair<ibv_gid, uint8_t>> queryRoCEv2GID(ibv_context *ctx, uint8_t portNum) {
  auto devname = ibv_get_device_name(ctx->device);
  for (uint8_t index = 0; index < 32; index++) {
    ibv_gid gid;
    auto ret = ibv_query_gid(ctx, portNum, index, &gid);
    if (ret < 0) {
      if (ret == ENODATA) {
        continue;
      }
      XLOGF(CRITICAL, "{}:{} failed to query GID {}", devname, portNum, index);
      return makeError(RPCCode::kIBOpenPortFailed, "query GID failed");
    }
    auto ip = folly::IPAddressV6::fromBinary(folly::ByteRange(gid.raw, sizeof(gid.raw)));
    if (ip.isZero()) {
      continue;
    }
    if (ip.isLinkLocal()) {
      XLOGF(DBG, "{}:{} skip GID {}, ip {}", devname, portNum, index, ip.str());
      continue;
    }
    auto path = Path(ctx->device->ibdev_path) / fmt::format("ports/{}/gid_attrs/types/{}", portNum, index);
    auto gidType = std::string();
    auto ok = folly::readFile(path.c_str(), gidType);
    if (!ok) {
      XLOGF(CRITICAL, "{}:{} failed to query GID type from sysfs {}", devname, portNum, path);
      return makeError(RPCCode::kIBOpenPortFailed, "query GID type failed");
    }
    if (!gidType.starts_with(V2_TYPE)) {
      XLOGF(DBG,
            "{}:{} skip GID {}, type {} != {}",
            devname,
            portNum,
            index,
            gidType.substr(0, gidType.find_first_of('\n')),
            V2_TYPE);
      continue;
    }

    XLOGF(DBG, "{}:{} found RoCE v2 GID, index {}, ip {}", devname, portNum, index, ip.str());
    return std::pair<ibv_gid, uint8_t>(gid, index);
  }

  XLOGF(ERR, "{}:{} RoCE v2 GID not found", devname, portNum);
  return makeError(RPCCode::kIBOpenPortFailed, "RoCE v2 GID not found");
}

}  // namespace

using IBDevPort = std::pair<std::string, uint8_t>;
using NetDev = std::string;

// Helper to get the PCI ID from a device path.
static std::optional<std::string> get_pci_id(const std::filesystem::path &path) {
  std::error_code ec;
  if (!std::filesystem::is_symlink(path, ec)) {
    return std::nullopt;
  }

  auto pci_path = std::filesystem::read_symlink(path, ec);
  if (ec) {
    return std::nullopt;
  }
  return pci_path.filename().string();
}

// Build a map from PCI bus ID to network device name.
// This is used as a fallback mechanism for RoCE devices and avoids repeatedly scanning /sys/class/net.
static std::map<std::string, std::string> build_pci_to_netdev_map() {
  std::map<std::string, std::string> pci_to_netdev;
  const std::filesystem::path net_path("/sys/class/net");
  std::error_code ec;

  if (!std::filesystem::exists(net_path, ec) || !std::filesystem::is_directory(net_path, ec)) {
    XLOGF(WARN, "Cannot open /sys/class/net directory, it may not exist.");
    return pci_to_netdev;
  }

  for (const auto &entry : std::filesystem::directory_iterator(net_path, ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    const auto device_path = entry.path() / "device";
    if (auto pci_id = get_pci_id(device_path)) {
      pci_to_netdev[*pci_id] = entry.path().filename().string();
    }
  }
  if (ec) {
    XLOGF(WARN, "Error iterating /sys/class/net: {}", ec.message());
  }
  return pci_to_netdev;
}

// Gets the mapping from ibdev to netdev from /sys/class/infiniband.  This is a more
// robust approach that does not depend on IPoIB or RoCE mode,  does not require special
// permissions, and is independent of the execution environment.
// It replaces the dependency on the 'ibdev2netdev' command-line tool.
static Result<std::map<IBDevPort, NetDev>> ibdev2netdev() {
  namespace fs = std::filesystem;
  const fs::path ib_base_path("/sys/class/infiniband");

  if (!fs::exists(ib_base_path)) {
    XLOGF(WARN, "Infiniband sysfs path {} does not exist. Cannot map IB devices.", ib_base_path.string());
    return std::map<IBDevPort, NetDev>();
  }

  // Pre-build a map from PCI ID to netdev name to avoid re-scanning /sys/class/net
  // for every RoCE device that needs the fallback method.
  auto pci_to_netdev_map = build_pci_to_netdev_map();
  std::map<IBDevPort, NetDev> map;
  std::error_code ec;

  for (const auto &ib_dev_entry : fs::directory_iterator(ib_base_path, ec)) {
    if (!ib_dev_entry.is_directory(ec)) {
      continue;
    }
    auto ib_dev_name = ib_dev_entry.path().filename().string();
    auto ports_path = ib_dev_entry.path() / "ports";

    if (!fs::exists(ports_path, ec) || !fs::is_directory(ports_path, ec)) {
      continue;
    }

    std::optional<std::string> pci_id;

    for (const auto &port_entry : fs::directory_iterator(ports_path, ec)) {
      if (!port_entry.is_directory(ec)) {
        continue;
      }

      auto port_str = port_entry.path().filename().string();
      auto port_num_res = folly::tryTo<uint8_t>(port_str);
      if (port_num_res.hasError()) {
        XLOGF(WARN, "Failed to parse port number from directory name: {}", port_str);
        continue;
      }
      uint8_t port_num = port_num_res.value();

      bool net_found = false;
      // Fast Path: Check for a 'net' subdirectory. This is the standard method for IPoIB
      // and modern RoCE drivers, providing a direct port-to-netdev mapping.
      auto net_path = port_entry.path() / "net";
      if (fs::exists(net_path, ec) && fs::is_directory(net_path, ec)) {
        for (const auto &net_dev_entry : fs::directory_iterator(net_path, ec)) {
          if (net_dev_entry.is_directory(ec)) {
            map[{ib_dev_name, port_num}] = net_dev_entry.path().filename().string();
            XLOGF(INFO,
                  "ibdev2netdev parsed from sysfs: {} port {} => {}",
                  ib_dev_name,
                  port_num,
                  map.at({ib_dev_name, port_num}));
            net_found = true;
            break;  // Assume one netdev per port
          }
        }
      }

      if (net_found) {
        continue;
      }

      // Fallback for RoCE: If the fast path fails, try to match the device's PCI ID.
      // This is common for older RoCE drivers that don't create the 'net' directory.
      if (!pci_id) {
        pci_id = get_pci_id(ib_dev_entry.path() / "device");
      }

      if (pci_id && pci_to_netdev_map.count(*pci_id)) {
        map[{ib_dev_name, port_num}] = pci_to_netdev_map.at(*pci_id);
        XLOGF(INFO,
              "ibdev2netdev parsed from PCI map: {} port {} => {}",
              ib_dev_name,
              port_num,
              map.at({ib_dev_name, port_num}));
      }
    }
  }

  if (ec) {
    XLOGF(ERR, "Error iterating sysfs for IB devices: {}", ec.message());
    return makeError(StatusCode::kIOError, "Filesystem error while mapping IB devices");
  }

  return map;
}

static bool comparePortAttr(ibv_port_attr a, ibv_port_attr b) {
  return a.link_layer == b.link_layer && a.state == b.state && a.lid == b.lid && a.active_mtu == b.active_mtu;
}

Result<IBConfig::Network> IBConfig::Network::from(std::string_view str) {
  try {
    auto [ip, mask] = folly::IPAddress::createNetwork(str);
    if (ip.isV4()) {
      return IBConfig::Network{folly::CIDRNetworkV4(ip.asV4(), mask)};
    }
    XLOGF(ERR, "Subnet {} is not IPV4", str);
  } catch (std::runtime_error &ex) {
    XLOGF(ERR, "Failed to parse subnet {}, exception: {}", str, ex.what());
  }
  return makeError(StatusCode::kInvalidArg);
}

/* IBDevice */
const std::vector<IBDevice::Ptr> &IBDevice::all() { return IBManager::instance().allDevices(); }

Result<std::vector<IBDevice::Ptr>> IBDevice::openAll(const IBConfig &config) {
  auto ib2net = ibdev2netdev();
  if (ib2net.hasError()) {
    XLOGF(WARN, "Failed to load ibdev2netdev, maybe running in container.");
    ib2net = std::map<IBDevPort, NetDev>();
  }

  auto ifaddrs = IfAddrs::load();
  if (ifaddrs.hasError()) {
    XLOGF(ERR, "Failed to load ifaddrs, error {}", ifaddrs.error());
    return makeError(RPCCode::kIBInitFailed);
  }

  int deviceCnt = 0;
  auto deviceList = ibv_get_device_list(&deviceCnt);
  if (deviceList == nullptr) {
    XLOGF(ERR, "Failed to load verbs device list, errno {}", errno);
    return makeError(RPCCode::kIBInitFailed, "Failed to load verbs devices.");
  }
  SCOPE_EXIT { ibv_free_device_list(deviceList); };
  if (deviceCnt <= 0) {
    XLOGF(WARN, "No RDMA devices!!!");
    return makeError(RPCCode::kIBInitFailed, "No usable RDMA devices.");
  }

  std::vector<IBDevice::Ptr> devices;
  for (int idx = 0; idx < deviceCnt; idx++) {
    auto dev = deviceList[idx];
    auto devId = devices.size();
    auto devName = ibv_get_device_name(dev);

    auto device = IBDevice::open(dev, devId, *ib2net, *ifaddrs, config);
    if (device.hasError()) {
      if (config.skip_unusable_device()) {
        XLOGF(CRITICAL, "Failed to open IBDevice {}, error {}, skip it", devName, device.error());
        continue;
      }
      XLOGF(ERR, "IBDevice failed to open device {}, error {}", devName, device.error());
      return makeError(device.error());
    }
    if (device.value()->ports().empty()) {
      XLOGF(INFO, "IBDevice skip {} because it doesn't have available ports.", devName);
      continue;
    }
    XLOGF(INFO, "IBDevice add {}, id {}, {} available ports", devName, devId, device.value()->ports().size());
    devices.emplace_back(std::move(*device));
  }

  if (devices.size() > kMaxDeviceCnt) {
    XLOGF(CRITICAL,
          "too many ibdevices {} > kMaxDeviceCnt {}, please specify device_filter",
          devices.size(),
          kMaxDeviceCnt);
    return makeError(StatusCode::kInvalidArg);
  }

  return devices;
}

Result<IBDevice::Ptr> IBDevice::open(ibv_device *dev,
                                     uint8_t devId,
                                     std::map<std::pair<std::string, uint8_t>, std::string> ib2net,
                                     std::multimap<std::string, IfAddrs::Addr> ifaddrs,
                                     const IBConfig &config) {
  IBDevice::Ptr device(new IBDevice());
  device->devId_ = devId;
  device->name_ = ibv_get_device_name(dev);
  device->context_.reset(ibv_open_device(dev));
  if (!device->context_) {
    XLOGF(ERR, "IBDevice failed to open {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }
  device->pd_.reset(ibv_alloc_pd(device->context_.get()));
  if (!device->pd_) {
    XLOGF(ERR, "IBDevice failed to alloc pd for {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }
  if (auto ret = ibv_query_device(device->context_.get(), &device->attr_); ret != 0) {
    XLOGF(ERR, "IBDevice failed to query device {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }

  auto filter = [&filter = config.device_filter()](std::string name) {
    return filter.empty() || std::find(filter.begin(), filter.end(), name) != filter.end();
  };

  std::set<uint8_t> ports;
  for (uint8_t portNum = 1; portNum <= device->attr_.phys_port_cnt; portNum++) {
    auto iter = ib2net.find({device->name_, portNum});
    auto netdev = (iter != ib2net.end()) ? std::optional(iter->second) : std::nullopt;
    if (filter(device->name_) || (netdev && filter(*netdev))) {
      ports.emplace(portNum);
    } else {
      XLOGF(INFO, "Skip device {}, port {} because it's not in device filter.", device->name_, portNum);
    }
  }

  auto flags = fcntl(device->context()->async_fd, F_GETFL);
  auto ret = fcntl(device->context()->async_fd, F_SETFL, flags | O_NONBLOCK);
  if (ret < 0) {
    XLOGF(ERR, "IBDevice {} failed to set async fd to NONBLOCK.", device->name());
    return makeError(RPCCode::kIBInitFailed);
  }

  for (uint8_t portNum = 1; portNum <= device->attr_.phys_port_cnt; portNum++) {
    if (!ports.contains(portNum)) {
      continue;
    }

    ibv_port_attr portAttr;
    if (auto ret = ibv_query_port(device->context_.get(), portNum, &portAttr); ret != 0) {
      XLOGF(ERR, "IBDevice failed to query port {} of device {}, errno {}", portNum, device->name_, ret);
      return makeError(RPCCode::kIBInitFailed);
    }
    if (portAttr.link_layer != IBV_LINK_LAYER_ETHERNET && portAttr.link_layer != IBV_LINK_LAYER_INFINIBAND) {
      XLOGF(WARN,
            "IBDevice skip port {} of device {}, linklayer {} is not RoCE or INFINIBAND.",
            portNum,
            device->name_,
            portAttr.link_layer);
      continue;
    }
    bool inactive = (portAttr.state != IBV_PORT_ACTIVE && portAttr.state != IBV_PORT_ACTIVE_DEFER);
    XLOGF_IF(WARN,
             inactive,
             "IBDevice {} port {} is not active, state {}, skip {}",
             device->name_,
             portNum,
             magic_enum::enum_name(portAttr.state),
             config.skip_inactive_ports());
    if (inactive && config.skip_inactive_ports()) {
      continue;
    }

    Port port;
    port.addrs = getIBPortAddrs(ib2net, ifaddrs, device->name_, portNum);
    port.zones = getZonesByAddrs(port.addrs, config, device->name_, portNum);
    port.attr = portAttr;
    if (!config.allow_unknown_zone() && port.zones == std::set<std::string>{std::string(IBConfig::kUnknownZone)}) {
      XLOGF(CRITICAL, "IBDevice {}:{}'s zone is unknown!!!", device->name_, portNum);
      return makeError(StatusCode::kInvalidConfig);
    }
    std::optional<ibv_gid> rocev2Gid;
    if (portAttr.link_layer == IBV_LINK_LAYER_ETHERNET) {
      auto result = queryRoCEv2GID(device->context(), portNum);
      RETURN_ON_ERROR(result);
      rocev2Gid = result->first;
    }
    XLOGF(INFO,
          "IBDevice {} add active port {}, linklayer {}, addrs {}, zones {}, RoCE v2 GID {}",
          device->name_,
          portNum,
          fmt::ibvLinklayerName(portAttr.link_layer),
          fmt::join(port.addrs.begin(), port.addrs.end(), ";"),
          fmt::join(port.zones.begin(), port.zones.end(), ";"),
          OptionalFmt(rocev2Gid));
    device->ports_[portNum] = std::move(port);
  }

  return device;
}

std::vector<IfAddrs::Addr> IBDevice::getIBPortAddrs(const IB2NetMap &ibdev2netdev,
                                                    const IfAddrs::Map &ifaddrs,
                                                    const std::string &devName,
                                                    uint8_t portNum) {
  auto key = IBDevPort{devName, portNum};
  if (!ibdev2netdev.contains(key)) {
    XLOGF(WARN, "IBDevice {}:{}'s netdev is unknown, maybe running in container.", devName, portNum);
    return {};
  }
  auto netdev = ibdev2netdev.at(key);
  auto [begin, end] = ifaddrs.equal_range(netdev);
  if (begin == end) {
    XLOGF(WARN, "IfAddr of {}:{} -> {} not found, maybe running in container!", devName, portNum, netdev);
    return {};
  }
  XLOGF_IF(WARN, begin == end, "IfAddr of {} not found!!", netdev);
  std::vector<IfAddrs::Addr> addrs;
  addrs.reserve(std::distance(begin, end));
  while (begin != end) {
    addrs.push_back(begin->second);
    begin++;
  }
  XLOGF(INFO, "IBDevice {}:{} IP addrs {}", devName, portNum, fmt::join(addrs.begin(), addrs.end(), ","));
  return addrs;
}

std::set<std::string> IBDevice::getZonesByAddrs(std::vector<IfAddrs::Addr> addrs,
                                                const IBConfig &config,
                                                const std::string &devName,
                                                uint8_t portNum) {
  std::set<std::string> zones;
  for (const auto addr : addrs) {
    for (const auto &subnet : config.subnets()) {
      if (addr.ip.inSubnet(subnet.subnet().ip(), subnet.subnet().mask())) {
        XLOGF(INFO,
              "IBDevice {}:{} addr {} in subnet {}, add network zones {}",
              devName,
              portNum,
              addr,
              subnet.subnet(),
              fmt::join(subnet.network_zones().begin(), subnet.network_zones().end(), ","));
        for (const auto &zone : subnet.network_zones()) {
          zones.insert(zone);
        }
      }
    }
  }

  if (zones.empty()) {
    auto fallback =
        config.default_network_zone().empty() ? std::string(IBConfig::kUnknownZone) : config.default_network_zone();
    if (fallback.starts_with("$")) {
      auto envName = fallback.substr(1);
      auto env = std::getenv(envName.c_str());
      if (!env || strlen(env) == 0) {
        XLOGF(CRITICAL,
              "IBDevice default network zone ENV {} not set, {}:{} fallback to {}!",
              envName,
              devName,
              portNum,
              IBConfig::kUnknownZone);
        return {std::string(IBConfig::kUnknownZone)};
      } else {
        XLOGF(INFO, "IBDevice {}:{} set to default zone {}, specified by ENV {}", devName, portNum, env, envName);
        return {std::string(env)};
      }
    }
    XLOGF(CRITICAL, "IBDevice {}:{} can't set zone by IP, fallback to {}", devName, portNum, fallback);
    return {fallback};
  }
  return zones;
}

Result<IBPort> IBDevice::openPort(size_t portNum) const {
  if (!ports_.contains(portNum)) {
    XLOGF(ERR, "IBDevice {} doesn't have port {}", name_, portNum);
    return makeError(RPCCode::kIBDeviceNotFound, fmt::format("port {}:{} not found", name_, portNum));
  }
  ibv_port_attr attr;
  if (auto ret = ibv_query_port(context(), portNum, &attr); ret != 0) {
    XLOGF(CRITICAL, "IBDevice failed to query port {} of device {}, errno {}", portNum, name_, ret);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("query port {}:{} failed, err {}", name_, portNum, ret));
  }
  if (!comparePortAttr(attr, *ports_.at(portNum).attr.rlock())) {
    RETURN_ON_ERROR(updatePort(portNum));
  }

  std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid;
  if (attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
    auto result = queryRoCEv2GID(context(), portNum);
    RETURN_ON_ERROR(result);
    rocev2Gid = *result;
  }

  return IBPort(shared_from_this(), portNum, attr, rocev2Gid);
}

Result<Void> IBDevice::updatePort(size_t portNum) const {
  if (!ports_.contains(portNum)) {
    XLOGF(ERR, "IBDevice {} doesn't have port {}", name_, portNum);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("port {}:{} not found", name_, portNum));
  }

  const auto &port = ports_.at(portNum);
  auto wlock = port.attr.wlock();
  ibv_port_attr attr;
  if (auto ret = ibv_query_port(context(), portNum, &attr); ret != 0) {
    XLOGF(CRITICAL, "IBDevice failed to query port {} of device {}, errno {}", portNum, name_, ret);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("query port {}:{} failed, err {}", name_, portNum, ret));
  }

  if (!comparePortAttr(attr, *wlock)) {
    if (attr.state != IBV_PORT_ACTIVE && attr.state != IBV_PORT_ACTIVE_DEFER) {
      XLOGF(WARN, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
    } else {
      XLOGF(INFO, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
    }
  } else {
    XLOGF(DBG, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
  }

  *wlock = attr;
  if (attr.state == IBV_PORT_ACTIVE || attr.state == IBV_PORT_ACTIVE_DEFER) {
    deviceCnt.set(1, getPortTags(*this, portNum, port.zones));
  } else {
    deviceCnt.set(-1, getPortTags(*this, portNum, port.zones));
  }

  return Void{};
}

ibv_mr *IBDevice::regMemory(void *addr, size_t length, int access) const {
  auto begin = SteadyClock::now();
  auto *mr = ibv_reg_mr(pd_.get(), addr, length, access);
  if (UNLIKELY(!mr)) {
    XLOGF(CRITICAL,
          "IBDevice {} failed to reg_mr, addr {}, length {}, access {}, errno {}",
          name(),
          addr,
          length,
          access,
          errno);
    mrError.addSample(1, {{"instance", name()}, {"tag", folly::to<std::string>(errno)}});
    return nullptr;
  }

  XLOGF(DBG, "IBDevice {} reg_mr, addr {}, length {}, access {}, mr {}", name(), addr, length, access, (void *)mr);
  auto tag = monitor::TagSet{{"instance", name()}};
  mrcnt.addSample(1, tag);
  mrSize.addSample(length, tag);
  mrLatency.addSample(SteadyClock::now() - begin, tag);
  return mr;
}

int IBDevice::deregMemory(ibv_mr *mr) const {
  auto length = mr ? mr->length : 0;
  auto ret = ibv_dereg_mr(mr);
  if (UNLIKELY(ret)) {
    XLOGF(CRITICAL, "IBDevice {} failed to dereg_mr {}, ret {}", name(), (void *)mr, ret);
    mrError.addSample(1, {{"instance", name()}, {"tag", folly::to<std::string>(ret)}});
    return ret;
  }

  XLOGF(DBG, "IBDevice {} dereg_mr mr {}", name(), (void *)mr);
  auto tag = monitor::TagSet{{"instance", name()}};
  mrcnt.addSample(-1, tag);
  mrSize.addSample(-length, tag);
  return ret;
}

class IBDevice::BackgroundRunner : public EventLoop::EventHandler,
                                   public std::enable_shared_from_this<BackgroundRunner> {
 public:
  static std::shared_ptr<BackgroundRunner> create() {
    auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
      XLOGF(ERR, "Failed to create timer fd, errno {}", errno);
      return {};
    }

#ifdef NDEBUG
#define IB_RUNNER_INTERVAL 15
#else
#define IB_RUNNER_INTERVAL 5
#endif
    static_assert(IB_RUNNER_INTERVAL > 0);

    itimerspec spec{{IB_RUNNER_INTERVAL, 0}, {IB_RUNNER_INTERVAL, 0}};
    auto ret = timerfd_settime(fd, 0, &spec, nullptr);
    XLOGF_IF(FATAL, ret != 0, "timerfd_settime failed, errno {}", errno);
    return std::make_shared<BackgroundRunner>(fd);
  }

  BackgroundRunner(int fd)
      : timer_(fd) {}

  int fd() const override { return timer_; }
  void handleEvents(uint32_t /* events */) override {
    std::array<char, 64> buf;
    while (true) {
      auto read = ::read(timer_, buf.data(), buf.size());
      if (read < (int)buf.size()) {
        break;
      }
    }

    for (auto &dev : IBDevice::all()) {
      for (auto &[portNum, port] : dev->ports()) {
        dev->updatePort(portNum);
      }
    }
  }

 private:
  FdWrapper timer_;
};

class IBDevice::AsyncEventHandler : public EventLoop::EventHandler {
 public:
  AsyncEventHandler(IBDevice::Ptr dev)
      : dev_(dev) {}

  int fd() const override { return dev_->context()->async_fd; }
  void handleEvents(uint32_t /* epollEvents */) override { return dev_->checkAsyncEvent(); }

 private:
  IBDevice::Ptr dev_;
};

void IBDevice::checkAsyncEvent() const {
  XLOGF(DBG, "IBDevice {} check async event.", name());

  ibv_async_event event;
  auto ret = ibv_get_async_event(context(), &event);
  if (ret != 0) {
    return;
  }
  SCOPE_EXIT { ibv_ack_async_event(&event); };

  auto eventName = magic_enum::enum_name(event.event_type);
  auto instance = std::string(eventName);
  switch (event.event_type) {
    case IBV_EVENT_DEVICE_FATAL:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}", name())}});
      XLOGF(CRITICAL, "IBDevice {} get event {}", name(), eventName);
      break;
    case IBV_EVENT_PORT_ERR:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}:{}", name(), event.element.port_num)}});
      XLOGF(CRITICAL, "IBDevice {}:{} get event {}", name(), event.element.port_num, eventName);
      updatePort(event.element.port_num);
      break;
    case IBV_EVENT_PORT_ACTIVE:
    case IBV_EVENT_LID_CHANGE:
    case IBV_EVENT_PKEY_CHANGE:
    case IBV_EVENT_SM_CHANGE:
    case IBV_EVENT_CLIENT_REREGISTER:
    case IBV_EVENT_GID_CHANGE:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}:{}", name(), event.element.port_num)}});
      XLOGF(INFO, "IBDevice {}:{} get event {}", name(), event.element.port_num, eventName);
      updatePort(event.element.port_num);
      break;
    default:
      XLOGF(DBG, "IBDevice {} get event {}", name(), eventName);
      break;
  }
}

/* IBPort */
IBPort::IBPort(std::shared_ptr<const IBDevice> dev,
               uint8_t portNum,
               ibv_port_attr attr,
               std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid)
    : dev_(dev),
      portNum_(portNum),
      attr_(attr),
      rocev2Gid_(rocev2Gid) {
  XLOGF_IF(FATAL, (dev && !dev->ports().contains(portNum)), "IBDevice {} doesn't have port {}!", dev->name(), portNum);
  XLOGF_IF(FATAL, (dev && isRoCE() && !rocev2Gid_), "{}:{} doesn't find RoCE v2 GID", dev->name(), portNum);
}

Result<ibv_gid> IBPort::queryGid(uint8_t index) const {
  ibv_gid gid;
  if (auto ret = ibv_query_gid(dev()->context(), portNum_, index, &gid); ret != 0) {
    XLOGF(ERR, "IBDevice {}:{} failed to query gid, errno {}", dev()->name(), portNum_, ret);
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  return gid;
}

/* IBManager */
void IBManager::close(std::unique_ptr<IBSocket> socket) { return instance().closeImpl(std::move(socket)); }

IBManager::IBManager() = default;
IBManager::~IBManager() { reset(); }

Result<Void> IBManager::startImpl(IBConfig config) {
  if (inited_) {
    return Void{};
  }

  static bool forkInited = false;
  if (!forkInited && config.fork_safe()) {
    auto ret = ibv_fork_init();
    if (ret < 0) {
      auto msg = fmt::format("ibv_fork_init failed {}", ret);
      XLOG(CRITICAL, msg);
      return makeError(RPCCode::kIBInitFailed, msg);
    }
    forkInited = true;
  }

  config_ = config;
  auto devices = IBDevice::openAll(config_);
  if (devices.hasError()) {
    XLOGF(ERR, "Failed to open all IBDevices, error {}", devices.error());
    return makeError(devices.error());
  } else if (devices->empty()) {
    if (config_.allow_no_usable_devices()) {
      XLOGF(WARN, "IBManager can't find available device!");
    } else {
      XLOGF(ERR, "IBManager can't find available device!");
      return makeError(RPCCode::kIBDeviceNotFound);
    }
  }

  devices_ = std::move(*devices);
  eventLoop_ = EventLoop::create();
  auto result = eventLoop_->start("IBManager");
  RETURN_ON_ERROR(result);

  socketManager_ = IBSocketManager::create();
  if (!socketManager_) {
    return makeError(RPCCode::kIBInitFailed, "Failed to create IBSocketManager");
  }
  eventLoop_->add(socketManager_, (EPOLLIN | EPOLLOUT | EPOLLET));

  devBgRunner_ = IBDevice::BackgroundRunner::create();
  if (!devBgRunner_) {
    return makeError(RPCCode::kIBInitFailed, "Failed to create IBDevice::BackgroundRunner");
  }
  eventLoop_->add(devBgRunner_, (EPOLLIN | EPOLLOUT | EPOLLET));

  for (const auto &dev : devices_) {
    auto handler = std::make_shared<IBDevice::AsyncEventHandler>(dev);
    eventLoop_->add(handler, (EPOLLIN | EPOLLOUT | EPOLLET));
    devEventHandlers_.push_back(handler);
    for (const auto &port : dev->ports()) {
      XLOGF_IF(FATAL, port.second.zones.empty(), "Zone is empty for port {}:{}", dev->name(), port.first);
      for (const auto &zone : port.second.zones) {
        if (zone != IBConfig::kUnknownZone) {
          zone2port_.emplace(zone, std::pair<IBDevice::Ptr, uint8_t>{dev, port.first});
        }
      }
      deviceCnt.set(1, getPortTags(*dev, port.first, port.second.zones));
    }
  }

  inited_ = true;
  return Void{};
}

void IBManager::reset() {
  inited_ = false;

  if (socketManager_) {
    socketManager_->stopAndJoin();
    socketManager_.reset();
  }
  if (eventLoop_) {
    eventLoop_->stopAndJoin();
    eventLoop_.reset();
  }

  devices_.clear();
  zone2port_.clear();
}

void IBManager::closeImpl(IBSocket::Ptr socket) {
  if (socketManager_) {
    socketManager_->close(std::move(socket));
  }
}

}  // namespace hf3fs::net