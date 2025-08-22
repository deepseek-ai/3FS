#pragma once

#include "client/cli/common/Dispatcher.h"
#include "fbs/mgmtd/ChainInfo.h"
#include "fbs/mgmtd/NodeInfo.h"

namespace hf3fs::client::cli {
void statNode(Dispatcher::OutputTable &table, const flat::NodeInfo &node);
void statChainInfo(Dispatcher::OutputTable &table, const flat::ChainInfo &chain);
}  // namespace hf3fs::client::cli
