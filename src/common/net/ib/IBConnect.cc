#include "common/net/ib/IBConnect.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/IPAddressV4.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <infiniband/verbs.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/net/Client.h"
#include "common/net/ib/IBConnectService.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/IBSocket.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/RobinHood.h"
#include "common/utils/SysResource.h"
#include "common/utils/UtcTime.h"

#ifdef NDEBUG
#define INJECT_DELAY()
#define INJECT_CONNECTION_LOST()
#else
#define INJECT_DELAY()                                               \
  do {                                                               \
    if (IBConnectService::delayMs() != 0) {                          \
      auto delay = folly::Random::rand32(delayMs());                 \
      co_await folly::coro::sleep(std::chrono::milliseconds(delay)); \
    }                                                                \
  } while (0)
#define INJECT_CONNECTION_LOST()                           \
  do {                                                     \
    if (IBConnectService::connectionLost()) {              \
      XLOGF(WARN, "Inject connection lost");               \
      co_return hf3fs::makeError(RPCCode::kConnectFailed); \
    }                                                      \
  } while (0)
#endif

#define CHECK_CONFIG_POSITIVE(v)                                                 \
  do {                                                                           \
    if ((v) <= 0) {                                                              \
      XLOGF(ERR, "IBSocket {} invalid config, " #v " {} <= 0", describe(), (v)); \
    }                                                                            \
  } while (0)

#define CHECK_CONFIG_LE(v1, v2)                                                                   \
  do {                                                                                            \
    if ((v1) > (v2)) {                                                                            \
      XLOGF(ERR, "IBSocket {} invalid config, " #v1 " {} > " #v2 " {}.", describe(), (v1), (v2)); \
      return -1;                                                                                  \
    }                                                                                             \
  } while (0)

namespace hf3fs::net {

namespace {
monitor::CountRecorder connecting("common.ib.connecting", {}, false);
monitor::CountRecorder accepting("common.ib.accepting", {}, false);
monitor::CountRecorder connectFailed("common.ib.connect_failed");
monitor::CountRecorder acceptedFailed("common.ib.accept_failed");

monitor::LatencyRecorder connectLatency("common.ib.connect_latency");
monitor::LatencyRecorder acceptLatency("common.ib.accept_latency");
}  // namespace

/* IBConnectService */
CoTryTask<IBQueryRsp> IBConnectService::query(serde::CallContext &ctx, const IBQueryReq &req) {
  INJECT_DELAY();
  XLOGF_IF(FATAL, !IBManager::initialized(), "IBDevice not initialized.");

  XLOGF(DBG, "IBConnectService::query get request from {}", ctx.peer());
  if (!ctx.transport()->isTCP()) {
    XLOGF(ERR, "IBConnectService::query from a non TCP transport!");
    co_return makeError(StatusCode::kInvalidArg);
  }

  IBQueryRsp rsp;
  for (const auto &dev : IBDevice::all()) {
    IBDeviceInfo devInfo;
    devInfo.dev = dev->id();
    devInfo.dev_name = dev->name();
    for (const auto &[portNum, port] : dev->ports()) {
      if (auto state = port.attr.rlock()->state; state != IBV_PORT_ACTIVE && state != IBV_PORT_ACTIVE_DEFER) {
        XLOGF(WARN, "Skip IBDevice {} port {}, state {}", dev->name(), portNum, static_cast<int>(state));
        continue;
      }
      auto &portInfo = devInfo.ports.emplace_back();
      portInfo.port = portNum;
      portInfo.link_layer = port.attr.rlock()->link_layer;
      for (const auto &zone : port.zones) {
        portInfo.zones.push_back(zone);
      }
    }
    if (devInfo.ports.empty()) {
      XLOGF(WARN, "Skip IBDevice {}, no active ports", devInfo.dev_name);
      continue;
    }
    rsp.devices.push_back(devInfo);
  }

  XLOGF(DBG, "IBConnectService::query return {}", rsp);

  co_return rsp;
}

CoTryTask<IBConnectRsp> IBConnectService::connect(serde::CallContext &ctx, const IBConnectReq &req) {
  INJECT_DELAY();
  XLOGF_IF(FATAL, !IBManager::initialized(), "IBDevice not initialized.");

  XLOGF(DBG,
        "IBConnectService::connect get request from {}, target device {}:{}",
        ctx.peer(),
        req.target_devname,
        req.target_port);
  if (!ctx.transport()->isTCP()) {
    XLOGF(ERR, "IBConnectService::connect from a non TCP transport!");
    co_return makeError(StatusCode::kInvalidConfig);
  }

  auto dev = IBDevice::get(req.target_dev);
  if (!dev || dev->name() != req.target_devname) {
    XLOGF(ERR, "IBConnectService can't device {} {}:{}", req.target_dev, req.target_devname, req.target_port);
    co_return makeError(RPCCode::kIBDeviceNotFound);
  }
  auto port = dev->openPort(req.target_port);
  if (port.hasError()) {
    XLOGF(CRITICAL,
          "IBConnectService::connect failed to open {}:{}, result {}",
          dev->name(),
          req.target_port,
          port.error());
    co_return makeError(RPCCode::kIBOpenPortFailed);
  }

  auto ibsocket = std::make_unique<IBSocket>(config_, std::move(*port));
  if (auto result = ibsocket->accept(ctx.transport()->peerIP(), req, acceptTimeout_()); result.hasError()) {
    XLOGF(ERR,
          "IBConnectService::connect failed to accept from {}, error {}",
          ctx.transport()->peerIP().str(),
          result.error());
    co_return makeError(result.error());
  }
  auto info = ibsocket->getConnectInfo();
  CO_RETURN_ON_ERROR(info);

  accept_(std::move(ibsocket));

  co_return IBConnectInfo(*info);
}

/* IBSocket */
struct MatchedDevice {
  uint8_t remote;
  uint8_t remotePort;
  uint8_t local;
  uint8_t localPort;
  std::string zone;

  std::string describe() const {
    if (zone.empty()) {
      return fmt::format("{{local {}:{} remote {}:{}}}", local, localPort, remote, remotePort);
    } else {
      return fmt::format("{{local {}:{}, remote {}:{}, zone {}}}", local, localPort, remote, remotePort, zone);
    }
  }
};

static bool checkDeviceMatch(const IBDeviceInfo::PortInfo &remotePort,
                             const IBDevice &local,
                             const IBDevice::Port &localPort,
                             int8_t localPortNum,
                             std::set<std::string> &zones) {
  std::set_intersection(remotePort.zones.begin(),
                        remotePort.zones.end(),
                        localPort.zones.begin(),
                        localPort.zones.end(),
                        std::inserter(zones, zones.begin()));

  auto attr = localPort.attr.rlock();
  if (attr->state != IBV_PORT_ACTIVE && attr->state != IBV_PORT_ACTIVE_DEFER) {
    XLOGF(WARN, "Skip inactive port {}:{}", local.name(), localPortNum);
    return false;
  }
  if (attr->link_layer != remotePort.link_layer) {
    XLOGF(WARN,
          "Skip port {}:{}, linklayer {} != {}",
          local.name(),
          localPortNum,
          fmt::ibvLinklayerName(remotePort.link_layer),
          fmt::ibvLinklayerName(attr->link_layer));
    return false;
  }
  return true;
}

void IBQueryRsp::findMatchDevices(const std::string &describe,
                                  std::vector<IBQueryRsp::Match> &ibMatches,
                                  std::vector<IBQueryRsp::Match> &roceMatches,
                                  bool checkZone) {
  for (const auto &remote : devices) {
    for (const auto &remotePort : remote.ports) {
      if (remotePort.link_layer != IBV_LINK_LAYER_INFINIBAND && remotePort.link_layer != IBV_LINK_LAYER_ETHERNET) {
        XLOGF(WARN, "Skip remote port {}:{} with invalid linklayer", remote.dev_name, remotePort.port);
        continue;
      }
      for (const auto &local : IBDevice::all()) {
        for (const auto &[localPortNum, localPort] : local->ports()) {
          std::set<std::string> zones;
          if (!checkDeviceMatch(remotePort, *local, localPort, localPortNum, zones)) {
            continue;
          }
          if (checkZone && zones.empty()) {
            continue;
          }
          IBQueryRsp::Match match;
          match.remoteDev = remote.dev;
          match.remoteDevName = remote.dev_name;
          match.remotePort = remotePort.port;
          match.localDev = local;
          match.localPort = localPortNum;
          match.zone = zones.empty() ? "" : *zones.begin();
          if (localPort.attr.rlock()->link_layer == IBV_LINK_LAYER_INFINIBAND) {
            ibMatches.push_back(match);
          } else {
            roceMatches.push_back(match);
          }
          XLOGF(DBG, "IBSocket {} find match devices {}", describe, match.describe());
        }
      }
    }
  }
}

Result<IBQueryRsp::Match> IBQueryRsp::selectDevice(const std::string &describe, uint64_t counter) {
  if (devices.empty()) {
    XLOGF(ERR, "IBSocket {} peer doesn't have active port", describe);
    return makeError(RPCCode::kIBDeviceNotFound, fmt::format("IBSocket {} peer doesn't have active port", describe));
  }

  for (auto checkZone : {true, false}) {
    if (!checkZone) {
      if (!IBManager::instance().config().allow_unknown_zone()) {
        break;
      } else {
        XLOGF(WARN, "IBSocket {} can't select device by zone, fallback to any zone.", describe);
      }
    }

    std::vector<Match> ibMatches;
    std::vector<Match> roceMatches;
    findMatchDevices(describe, ibMatches, roceMatches, checkZone);

    std::vector<Match> allMatches;
    allMatches.insert(allMatches.end(), ibMatches.begin(), ibMatches.end());
    allMatches.insert(allMatches.end(), roceMatches.begin(), roceMatches.end());
    if (!allMatches.empty()) {
      auto idx = counter % allMatches.size();
      if (!ibMatches.empty() && IBManager::instance().config().prefer_ibdevice()) {
        idx = counter % ibMatches.size();
      }
      auto match = allMatches[idx];
      XLOGF(DBG, "IBSocket {} use devices {}", describe, match.describe());
      return match;
    }
  }

  XLOGF(ERR, "IBSocket {} can't find match device to connect.", describe);
  return makeError(RPCCode::kIBDeviceNotFound, "No match device to connect");
}

Result<IBConnectInfo> IBSocket::getConnectInfo() const {
  static auto hostname = SysResource::hostname().value_or("unknown");
  IBConnectInfo info;
  info.hostname = hostname;
  info.dev = port_.dev()->id();
  info.dev_name = port_.dev()->name();
  info.port = port_.portNum();

  info.mtu = port_.attr().active_mtu;
  info.qp_num = qp_->qp_num;
  if (port_.isInfiniband()) {
    info.linklayer = IBConnectIBInfo{port_.attr().lid};
  } else if (port_.isRoCE()) {
    info.linklayer = IBConnectRoCEInfo{port_.getRoCEv2Gid().first};
  } else {
    XLOGF(ERR, "IBSocket port {} unknown linklayer!", port_);
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  return info;
}

Result<Void> IBSocket::checkPort() const {
  if (!port_) {
    XLOGF(ERR, "IBSocket port {} is not set", port_);
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  if (!port_.isActive()) {
    XLOGF(ERR, "IBSocket port {} is not valiable, state {}", port_, static_cast<int>(port_.attr().state));
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  if (!port_.isInfiniband() && !port_.isRoCE()) {
    XLOGF(ERR, "IBSocket port {} is not IB or RoCE", port_);
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  return Void{};
}

static folly::ConcurrentHashMap<Address, RelativeTime> lastLogErrorTime;

CoTryTask<void> IBSocket::connect(serde::ClientContext &ctx, Duration timeout) {
  auto begin = SteadyClock::now();
  connecting.addSample(1);
  SCOPE_EXIT {
    connecting.addSample(-1);
    connectLatency.addSample(SteadyClock::now() - begin);
  };
  auto failedGuard = folly::makeGuard([]() { connectFailed.addSample(1); });

  if (!IBManager::initialized()) {
    XLOGF(CRITICAL, "IBDevice not initialized!");
    co_return makeError(RPCCode::kIBDeviceNotInitialized);
  }

  net::UserRequestOptions opts;
  opts.timeout = timeout;
  auto addr = ctx.addr();
  {
    auto des = describe_.wlock();
    *des = fmt::format("[connect RDMA://{}]", addr.toFollyAddress().describe());
  }
  if (addr.isRDMA()) {
    XLOGF(FATAL, "IBSocket::connect should call with a TCP net client");
  }

  IBQueryReq query;
  auto queryRsp = co_await IBConnect<>::query(ctx, query, &opts);
  if (queryRsp.hasError()) {
    auto now = RelativeTime::now();
    if (now - lastLogErrorTime[addr] >= 10_s) {
      XLOGF(ERR, "IBSocket {} failed to query, error {}, timeout {}", describe(), queryRsp.error(), timeout);
      lastLogErrorTime.insert_or_assign(addr, now);
    }
    co_return makeError(std::move(queryRsp.error()));
  }

  uint64_t cnt = 0;
  {
    auto guard = config_.roundRobin_.wlock();
    if (auto iter = guard->find(addr); iter != guard->end()) {
      cnt = ++iter->second;
    } else {
      cnt = folly::Random::rand64();
      guard->insert({addr, cnt});
    }
  }
  auto match = queryRsp->selectDevice(describe(), cnt);
  CO_RETURN_ON_ERROR(match);
  XLOGF(INFO, "IBSocket {} connect with dev {}", describe(), match->describe());

  auto port = match->localDev->openPort(match->localPort);
  CO_RETURN_ON_ERROR(port);
  port_ = std::move(*port);
  CO_RETURN_ON_ERROR(checkPort());

  connectConfig_ = config_.clone().toIBConnectConfig(port_.isRoCE());
  if (checkConfig() != 0) {
    co_return makeError(StatusCode::kInvalidConfig, "IBSocket invalid configuration");
  }
  if (qpCreate() != 0) {
    co_return makeError(RPCCode::kConnectFailed, "IBSocket failed to create QP.");
  }
  if (qpInit() != 0) {
    co_return makeError(RPCCode::kConnectFailed, "IBSocket failed to init QP.");
  }
  // set state to CONNECTING
  auto old = state_.exchange(State::CONNECTING);
  XLOGF_IF(FATAL, old != State::INIT, "IBSocket {} old state {} != INIT", describe(), magic_enum::enum_name(old));

  if (UNLIKELY(closed_)) {
    XLOGF(WARN, "IBSocket {} closed before connected!", describe());
    co_return makeError(RPCCode::kSocketClosed);
  }

  auto info = getConnectInfo();
  CO_RETURN_ON_ERROR(info);

  IBConnectReq connect(*info, match->remoteDev, match->remoteDevName, match->remotePort, connectConfig_);
  auto connectRsp = co_await IBConnect<>::connect(ctx, connect, &opts);
  if (connectRsp.hasError()) {
    XLOGF(ERR, "IBSocket {} failed to connect, error {}, timeout {}", describe(), connectRsp.error(), timeout);
    co_return makeError(std::move(connectRsp.error()));
  }

  INJECT_CONNECTION_LOST();

  setPeerInfo(folly::IPAddressV4::fromLong(ctx.addr().ip), *connectRsp);
  if (qpReadyToRecv() != 0) {
    if (closed_) {
      co_return makeError(RPCCode::kSocketClosed, "IBSocket closed before connected");
    }
    co_return makeError(RPCCode::kConnectFailed);
  }
  if (qpReadyToSend() != 0) {
    if (closed_) {
      co_return makeError(RPCCode::kSocketClosed, "IBSocket closed before connected");
    }
    co_return makeError(RPCCode::kConnectFailed);
  }

  // post a empty msg to generate a event and notify remote side,
  // after this msg send success, state will change to State::READY
  auto bufIdx = sendBufs_.front().first;
  sendBufs_.pop();
  if (auto ret = postSend(bufIdx, 0, IBV_SEND_SIGNALED) != 0; ret != 0) {
    XLOGF(CRITICAL, "IBSocket {} failed to send empty msg, errno {}", describe(), ret);
    co_return Void{};
  }
  XLOGF(INFO, "IBSocket {} connected", describe());

  failedGuard.dismiss();

  co_return Void{};
}

Result<Void> IBSocket::accept(folly::IPAddressV4 ip, const IBConnectReq &req, Duration acceptTimeout) {
  auto begin = SteadyClock::now();
  accepting.addSample(1);
  SCOPE_EXIT {
    accepting.addSample(-1);
    acceptLatency.addSample(SteadyClock::now() - begin);
  };
  auto failedGuard = folly::makeGuard([]() { acceptedFailed.addSample(1); });

  RETURN_ON_ERROR(checkPort());

  {
    auto des = describe_.wlock();
    *des = fmt::format("[accept RDMA://{}]", ip.str());
  }
  connectConfig_ = req.config;
  if (checkConfig() != 0) {
    return makeError(StatusCode::kInvalidConfig);
  }

  // create QP, init QP and modify to RTR.
  if (qpCreate() != 0) {
    return makeError(RPCCode::kConnectFailed, "IBSocket failed to create QP");
  }
  if (qpInit() != 0) {
    return makeError(RPCCode::kConnectFailed, "IBSocket failed to init QP.");
  }
  setPeerInfo(ip, req);
  if (qpReadyToRecv() != 0) {
    if (closed_) {
      return makeError(RPCCode::kSocketClosed, "IBSocket closed before connected");
    }
    return makeError(RPCCode::kConnectFailed, "IBSocket failed to modify QP to RTR.");
  }

  // client will send a empty msg to server, then server's QP will turn to READY
  auto state = state_.exchange(State::ACCEPTED);
  XLOGF_IF(FATAL, state != State::INIT, "State is not INIT!!!");
  if (UNLIKELY(closed_)) {
    XLOGF(WARN, "IBSocket {} closed before accepted!", describe());
    return makeError(RPCCode::kSocketClosed);
  }
  XLOGF(INFO, "IBSocket {} accepted", describe());

  failedGuard.dismiss();
  acceptTimeout_ = SteadyClock::now() + acceptTimeout;

  return Void{};
}

void IBSocket::setPeerInfo(folly::IPAddressV4 ip, const IBConnectInfo &info) {
  peerIP_ = ip;
  peerInfo_ = info;

  // use instance to store local dev info, use tag to store remote hostname
  auto local = fmt::format("{}", port_.dev()->name());
  auto peer = info.hostname.empty() ? ip.str() : info.hostname;
  tag_ = monitor::TagSet{{"instance", local}};
  peerTag_ = monitor::TagSet{{"instance", local}, {"tag", peer}};

  {
    auto des = describe_.wlock();
    *des = fmt::format("RDMA://{}/{}:{}/{}", peer, info.dev_name, info.port, info.qp_num);
  }
}

int IBSocket::checkConfig() const {
  XLOGF_IF(FATAL, device() == nullptr, "device() == nullptr");

  CHECK_CONFIG_POSITIVE(connectConfig_.max_sge);
  CHECK_CONFIG_POSITIVE(connectConfig_.max_rdma_wr);
  CHECK_CONFIG_POSITIVE(connectConfig_.max_rdma_wr_per_post);
  CHECK_CONFIG_POSITIVE(connectConfig_.max_rd_atomic);
  CHECK_CONFIG_POSITIVE(connectConfig_.buf_size);
  CHECK_CONFIG_POSITIVE(connectConfig_.send_buf_cnt);
  CHECK_CONFIG_POSITIVE(connectConfig_.buf_ack_batch);
  CHECK_CONFIG_POSITIVE(connectConfig_.buf_signal_batch);
  CHECK_CONFIG_POSITIVE(connectConfig_.event_ack_batch);
  CHECK_CONFIG_POSITIVE(connectConfig_.buf_ack_batch);

  CHECK_CONFIG_LE(connectConfig_.max_sge, (uint32_t)device()->attr().max_sge);
  CHECK_CONFIG_LE(connectConfig_.max_rdma_wr_per_post, connectConfig_.max_rdma_wr);
  CHECK_CONFIG_LE(connectConfig_.max_rd_atomic, (uint32_t)device()->attr().max_qp_rd_atom);
  CHECK_CONFIG_LE(connectConfig_.qpMaxSendWR(), (uint32_t)device()->attr().max_qp_wr);
  CHECK_CONFIG_LE(connectConfig_.qpMaxCQEntries(), (uint32_t)device()->attr().max_cqe);
  CHECK_CONFIG_LE(connectConfig_.buf_ack_batch, connectConfig_.send_buf_cnt);
  CHECK_CONFIG_LE(connectConfig_.buf_signal_batch, connectConfig_.send_buf_cnt);

  return 0;
}

int IBSocket::qpCreate() {
  XLOGF(DBG, "IBSocket {} create QP.", describe());
  if (!port_.isActive()) {
    XLOGF(ERR, "IBSocket {} dev {}:{} not active", describe(), device()->name(), port_.portNum());
    return -1;
  }

  channel_.reset(ibv_create_comp_channel(device()->context()));
  if (UNLIKELY(!channel_)) {
    XLOGF(ERR, "IBSocket {} failed to create completion channel, errno {}", describe(), errno);
    return -1;
  }
  int flags = fcntl(channel_->fd, F_GETFL);
  if (UNLIKELY(flags == -1)) {
    XLOGF(ERR, "IBSocket {} failed to get fd flags of completion channel, errno {}", describe(), errno);
    return -1;
  }
  if (UNLIKELY(fcntl(channel_->fd, F_SETFL, flags | O_NONBLOCK))) {
    XLOGF(ERR, "IBSocket {} failed to set O_NONBLOCK on fd of completion channel, errno {}", describe(), errno);
    return -1;
  }

  cq_.reset(ibv_create_cq(device()->context(),
                          connectConfig_.qpMaxCQEntries(),
                          nullptr,
                          channel_.get(),
                          0 /* comp vector */));
  if (UNLIKELY(!cq_)) {
    XLOGF(ERR, "IBSocket {} failed to create CQ, errno {}", describe(), errno);
    return -1;
  }

  int ret = ibv_req_notify_cq(cq_.get(), 0);
  if (UNLIKELY(ret)) {
    XLOGF(ERR, "IBSocket {} failed to request notify CQ, errno {}", describe(), ret);
    return -1;
  }

  ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_context = nullptr;
  attr.send_cq = cq_.get();
  attr.recv_cq = cq_.get();
  attr.srq = nullptr;
  attr.cap.max_send_wr = connectConfig_.qpMaxSendWR();
  attr.cap.max_recv_wr = connectConfig_.qpMaxRecvWR();
  attr.cap.max_send_sge = connectConfig_.max_sge;
  attr.cap.max_recv_sge = 1;
  attr.cap.max_inline_data = 0;
  attr.qp_type = IBV_QPT_RC;
  attr.sq_sig_all = 0;

  qp_.reset(ibv_create_qp(device()->pd(), &attr));
  if (UNLIKELY(!qp_)) {
    XLOGF(ERR, "IBSocket {} failed to create QP, errno {}", describe(), errno);
    return -1;
  }

  return initBufs();
}

int IBSocket::qpInit() {
  XLOGF(DBG, "IBSocket {} init QP.", describe());

  ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
  attr.pkey_index = connectConfig_.pkey_index;
  attr.port_num = port_.portNum();

  int ret = ibv_modify_qp(qp_.get(), &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
  if (ret != 0) {
    XLOGF(ERR, "IBSocket {} failed to init QP, errno {}", describe(), errno);
    return -1;
  }

  return 0;
}

int IBSocket::qpReadyToRecv() {
  XLOGF(DBG, "IBSocket {} modify QP to RTR.", describe());

  ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = std::min(port_.attr().active_mtu, (enum ibv_mtu)peerInfo_.mtu);
  attr.dest_qp_num = peerInfo_.qp_num;
  attr.rq_psn = connectConfig_.start_psn;
  attr.max_dest_rd_atomic = connectConfig_.max_rd_atomic;
  attr.min_rnr_timer = connectConfig_.min_rnr_timer;

  if (port_.attr().link_layer != peerInfo_.getLinkerLayerType()) {
    XLOGF(ERR,
          "IBSocket local linklayer {} != remote linklayer {}",
          fmt::ibvLinklayerName(port_.attr().link_layer),
          fmt::ibvLinklayerName(static_cast<int>(peerInfo_.getLinkerLayerType())));
    return -1;
  }
  if (port_.attr().link_layer == IBV_LINK_LAYER_INFINIBAND) {
    auto conn = std::get<IBConnectIBInfo>(peerInfo_.linklayer);
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = conn.lid;
  } else if (port_.attr().link_layer == IBV_LINK_LAYER_ETHERNET) {
    auto conn = std::get<IBConnectRoCEInfo>(peerInfo_.linklayer);
    XLOGF(DBG,
          "IBSocket {} modify QP to RTR, linklayer ETHERNET, dgid {}, tc {}",
          describe(),
          conn.gid,
          connectConfig_.traffic_class);
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = conn.gid;
    attr.ah_attr.grh.flow_label = 0;
    attr.ah_attr.grh.sgid_index = port_.getRoCEv2Gid().second;
    attr.ah_attr.grh.hop_limit = 255;
    attr.ah_attr.grh.traffic_class = connectConfig_.traffic_class;
  } else {
    assert(false);
    XLOGF(ERR, "IBSocket unknown linklayer {}", port_.attr().link_layer);
    return -1;
  }

  attr.ah_attr.sl = connectConfig_.sl;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = peerInfo_.port;
  int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
             IBV_QP_MIN_RNR_TIMER;
  int ret = ibv_modify_qp(qp_.get(), &attr, mask);
  if (ret != 0) {
    if (closed_) {
      XLOGF(WARN, "IBSocket {} closed before connected, can't modify QP to RTR, errno {}", describe(), ret);
    } else {
      XLOGF(ERR,
            "IBSocket {} failed to modify QP to RTR, errno {}, peer {}, config {}",
            describe(),
            ret,
            peerInfo_,
            connectConfig_);
    }
    return -1;
  }

  // post receive buffer
  for (size_t idx = 0; idx < recvBufs_.getBufCnt(); idx++) {
    if (UNLIKELY(postRecv(idx))) {
      return -1;
    }
  }

  return 0;
}

int IBSocket::qpReadyToSend() {
  XLOGF(DBG, "IBSocket {} modify QP to RTS.", describe());

  ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = connectConfig_.timeout;
  attr.retry_cnt = connectConfig_.retry_cnt;
  attr.rnr_retry = connectConfig_.rnr_retry;
  attr.sq_psn = connectConfig_.start_psn;
  attr.max_rd_atomic = connectConfig_.max_rd_atomic;
  int ret = ibv_modify_qp(
      qp_.get(),
      &attr,
      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
  if (ret != 0) {
    if (closed_) {
      XLOGF(WARN, "IBSocket {} closed before connected, can't modify QP to RTS, errno {}", describe(), errno);
    } else {
      XLOGF(ERR, "IBSocket {} failed to modify QP to RTS, errno {}", describe(), errno);
    }
    return -1;
  }
  sendBufs_.push(sendBufs_.getBufCnt());
  rdmaSem_.signal(connectConfig_.max_rdma_wr);
  ackBufAvailable_.store(connectConfig_.qpAckBufs());

  return 0;
}

int IBSocket::initBufs() {
  XLOGF(DBG, "IBSocket {} init socket buffers.", describe());

  size_t bufSize = connectConfig_.buf_size;
  size_t numSend = connectConfig_.send_buf_cnt;
  size_t numRecv = connectConfig_.qpMaxRecvWR();
  auto bufMem =
      BufferMem::create(device(), bufSize, numSend + numRecv, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_RELAXED_ORDERING);
  if (bufMem.hasError()) {
    return -1;
  }
  mem_.emplace(std::move(*bufMem));

  sendBufs_.init(mem_->ptr, mem_->mr, bufSize, numSend);
  recvBufs_.init(mem_->ptr + bufSize * numSend, mem_->mr, bufSize, numRecv);

  return 0;
}

}  // namespace hf3fs::net
