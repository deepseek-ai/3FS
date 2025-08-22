#pragma once

#include <cstddef>
#include <cstdint>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/logging/xlog.h>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::meta::server {

class ChainAllocator {
 public:
  ChainAllocator(std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient)
      : mgmtdClient_(std::move(mgmtdClient)) {}

  CoTryTask<void> checkLayoutValid(const Layout &layout) {
    CO_RETURN_ON_ERROR(layout.valid(true));

    if (!layout.empty()) {
      auto routing = getRoutingInfo();

      const auto &chains = layout.getChainIndexList();
      for (auto index : chains) {
        auto ref = flat::ChainRef{layout.tableId, layout.tableVersion, index};
        if (auto chain = routing->getChain(ref); !chain) {
          XLOGF(ERR, "Layout contains a not found ChainRef {}", ref);
          co_return makeError(MetaCode::kInvalidFileLayout, fmt::format("{} not found", ref));
        } else if (chain->targets.empty()) {
          XLOGF(ERR, "Chain {} has no target", chain->chainId);
          co_return makeError(MetaCode::kInvalidFileLayout, fmt::format("Chain {} has no target", chain->chainId));
        }
      }
    }
    co_return Void{};
  }

  CoTryTask<void> allocateChainsForLayout(Layout &layout) {
    co_return co_await allocateChainsForLayout(layout, [&](size_t chainCnt) {
      auto tableId = layout.tableId;
      auto stripeSize = layout.stripeSize;
      auto key = AllocType(tableId, stripeSize);
      auto guard = roundRobin_.lock();
      auto iter = guard->find(key);
      if (iter == guard->end()) {
        // start with random value
        auto initial = folly::Random::rand32(chainCnt) / stripeSize * stripeSize;
        iter = guard->insert({key, initial}).first;
      }
      auto res = (iter->second % chainCnt) + 1;
      iter->second = (iter->second + stripeSize) % chainCnt;
      return res;
    });
  }

  CoTryTask<void> allocateChainsForLayout(Layout &layout, folly::Synchronized<uint32_t> &chainAllocCounter) {
    co_return co_await allocateChainsForLayout(layout, [&](size_t chainCnt) {
      auto guard = chainAllocCounter.wlock();
      auto stripeSize = layout.stripeSize;
      if (*guard == (uint32_t)-1) {
        // start with random value
        *guard = folly::Random::rand32(chainCnt) / stripeSize * stripeSize;
      }
      // add and return.
      auto res = (*guard % chainCnt) + 1;
      *guard = (*guard + stripeSize) % chainCnt;
      return res;
    });
  }

  CoTryTask<void> allocateChainsForLayout(Layout &layout, auto &&roundRobin) {
    CO_RETURN_ON_ERROR(co_await checkLayoutValid(layout));
    if (!layout.empty()) {
      co_return Void{};
    }

    auto tableId = layout.tableId;
    auto tableVersion = layout.tableVersion;

    auto routing = getRoutingInfo();
    const auto *table = routing->raw()->getChainTable(tableId, tableVersion);
    if (!table) {
      XLOGF(ERR, "Failed to find ChainTable with {} and {}", tableId, tableVersion);
      co_return makeError(MetaCode::kInvalidFileLayout,
                          fmt::format("ChainTable with {} and {} not found", tableId, tableVersion));
    } else if (!table->chainTableVersion) {
      XLOGF(ERR, "Invalid table {} version {}", tableId, tableVersion);
      co_return makeError(MetaCode::kInvalidFileLayout,
                          fmt::format("Invalid chain table {} version {}", tableId, tableVersion));
    }
    auto chainCnt = table->chains.size();
    if (chainCnt < layout.stripeSize || chainCnt == 0) {
      XLOGF(ERR,
            "Failed to allocate for layout {}, chain table {} have only {} chains.",
            layout,
            tableId.toUnderType(),
            chainCnt);
      co_return makeError(
          MetaCode::kInvalidFileLayout,
          fmt::format("try to allocate {} chains from {}, found {}", layout.stripeSize, tableId, chainCnt));
    }
    auto chainBegin = roundRobin(chainCnt);
    layout.tableVersion = table->chainTableVersion;
    layout.chains = Layout::ChainRange(chainBegin, Layout::ChainRange::STD_SHUFFLE_MT19937, folly::Random::rand64());
    if (auto valid = layout.valid(false); valid.hasError()) {
      XLOGF(DFATAL, "Layout is not valid after alloc {}, error {}", layout, valid.error());
      CO_RETURN_ERROR(valid);
    }

    co_return Void{};
  }

 private:
  std::shared_ptr<client::RoutingInfo> getRoutingInfo() { return mgmtdClient_->getRoutingInfo(); }

  using AllocType = std::pair<flat::ChainTableId, size_t>;
  folly::Synchronized<std::map<AllocType, uint32_t>, std::mutex> roundRobin_;
  std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient_;
};

}  // namespace hf3fs::meta::server
