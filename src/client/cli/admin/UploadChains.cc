#include "UploadChains.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/RapidCsv.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("upload-chains");
  parser.add_argument("-d", "--dump-template").default_value(false).implicit_value(true);
  parser.add_argument("csv-file-path");
  parser.add_argument("--set-preferred-target-order").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleUploadChains(IEnv &ienv,
                                                      const argparse::ArgumentParser &parser,
                                                      const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto csvFilePath = parser.get<std::string>("csv-file-path");
  if (parser.get<bool>("-d")) {
    std::ofstream of(csvFilePath);
    of.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    of << "ChainId,TargetId,TargetId\n";
    of << "123,100001,100101\n";
    of << "234,100002,100102\n";
    table.push_back({fmt::format("Dump template to {} succeeded", csvFilePath)});
    co_return table;
  }

  auto setPreferredTargetOrder = parser.get<bool>("--set-preferred-target-order");

  rapidcsv::Document doc(csvFilePath);
  auto columnNames = doc.GetColumnNames();

  if (columnNames.empty()) {
    co_return makeError(StatusCode::kInvalidFormat, "empty columns");
  }

  if (columnNames[0] != "ChainId") {
    co_return makeError(StatusCode::kInvalidFormat,
                        fmt::format("column[0] expected:'ChainId' actual:'{}'", columnNames[0]));
  }

  if (columnNames.size() == 1) {
    co_return makeError(StatusCode::kInvalidFormat, "target columns not found");
  }

  for (size_t i = 1; i < columnNames.size(); ++i) {
    if (columnNames[i] != "TargetId") {
      co_return makeError(StatusCode::kInvalidFormat,
                          fmt::format("column[{}] expected:'TargetId' actual:'{}'", i, columnNames[i]));
    }
  }

  if (doc.GetRowCount() == 0) {
    co_return makeError(StatusCode::kInvalidFormat, "empty rows");
  }

  std::vector<flat::ChainSetting> chains;
  for (size_t i = 0; i < doc.GetRowCount(); ++i) {
    auto row = doc.GetRow<int64_t>(i);
    if (row.size() != columnNames.size()) {
      co_return makeError(
          StatusCode::kInvalidFormat,
          fmt::format("unexpected size of row[{}]. expected:{} actual:{}", i, columnNames.size(), row.size()));
    }

    flat::ChainSetting chain;
    chain.chainId = flat::ChainId(row[0]);
    chain.setPreferredTargetOrder = setPreferredTargetOrder;
    for (size_t j = 1; j < row.size(); ++j) {
      if (row[j] <= 0) {
        co_return makeError(StatusCode::kInvalidFormat,
                            fmt::format("invalid TargetId of row[{}][{}]:{}", i, j, row[j]));
      }
      flat::ChainTargetSetting target;
      target.targetId = flat::TargetId(row[j]);
      chain.targets.push_back(target);
    }
    chains.push_back(std::move(chain));
  }

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->setChains(env.userInfo, chains));

  table.push_back({fmt::format("Upload {} chains succeeded", chains.size())});
  co_return table;
}
}  // namespace
CoTryTask<void> registerUploadChainsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleUploadChains);
}
}  // namespace hf3fs::client::cli
