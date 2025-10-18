#pragma once

#include <folly/Conv.h>
#include <optional>

#include "client/cli/common/Utils.h"
#include "common/utils/ArgParse.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/ChainRef.h"

namespace hf3fs::client::cli {

inline void addLayoutArguments(argparse::ArgumentParser &parser) {
  parser.add_argument("--chain-table-id").scan<'u', uint32_t>();
  parser.add_argument("--chain-table-ver").scan<'u', uint32_t>();
  parser.add_argument("--chain-list").help("Chain IDs separated by ',', eg: 1,2,3,4,5, can use with chain-table");
  parser.add_argument("--chunk-size");
  parser.add_argument("--stripe-size").scan<'u', uint32_t>();
}

inline std::optional<meta::Layout> parseLayout(const argparse::ArgumentParser &parser,
                                               std::optional<meta::Layout> base = {}) {
  auto chainTableId = parser.present<uint32_t>("--chain-table-id");
  auto chainTableVersion = parser.present<uint32_t>("--chain-table-ver");
  auto chainList = parser.present<std::string>("--chain-list");
  std::optional<uint32_t> chunkSize;
  if (auto l = parser.present<std::string>("--chunk-size")) {
    chunkSize = Size::from(*l).value();
  }
  auto stripeSize = parser.present<uint32_t>("--stripe-size");
  size_t args = chainTableId.has_value() + chainTableVersion.has_value() + chainList.has_value() +
                chunkSize.has_value() + stripeSize.has_value();
  if (args == 0) {
    return std::nullopt;
  }
  if (base.has_value() && !chainList.has_value() && !chainTableId.has_value() && !chainTableVersion.has_value()) {
    base->chunkSize = chunkSize.value_or(base->chunkSize);
    base->stripeSize = stripeSize.value_or(base->stripeSize);
    return *base;
  }
  ENSURE_USAGE(chunkSize.has_value() && (stripeSize.has_value() || chainList.has_value()) &&
               (chainList.has_value() || chainTableId.has_value()));
  if (chainList.has_value()) {
    std::vector<std::string> idList;
    folly::split(',', *chainList, idList, true);
    ENSURE_USAGE(!idList.empty(), "chain-list is empty");
    ENSURE_USAGE(!stripeSize.has_value() || *stripeSize == idList.size(),
                 fmt::format("chain-list and stripe-size not match, len({}) == {}, stripe {}",
                             *chainList,
                             idList.size(),
                             *stripeSize));
    std::vector<uint32_t> chains;
    for (const auto &id : idList) {
      auto chain = folly::tryTo<uint32_t>(id);
      ENSURE_USAGE(chain.hasValue(), fmt::format("Failed to parse chain-list {}, element {}", *chainList, id));
      chains.push_back(*chain);
    }
    return meta::Layout::newChainList(flat::ChainTableId(chainTableId.value_or(0)),
                                      flat::ChainTableVersion(chainTableVersion.value_or(0)),
                                      *chunkSize,
                                      std::move(chains));
  } else {
    return meta::Layout::newEmpty(flat::ChainTableId(*chainTableId), 
                                  flat::ChainTableVersion(chainTableVersion.value_or(0)),
                                  *chunkSize, *stripeSize);
  }
}

}  // namespace hf3fs::client::cli
