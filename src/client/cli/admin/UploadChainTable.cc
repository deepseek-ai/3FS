#include "UploadChainTable.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/RapidCsv.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("upload-chain-table");
  parser.add_argument("-d", "--dump-template").default_value(false).implicit_value(true);
  parser.add_argument("tableId").scan<'u', uint32_t>();
  parser.add_argument("csv-file-path");
  parser.add_argument("--desc");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleUploadChainTable(IEnv &ienv,
                                                          const argparse::ArgumentParser &parser,
                                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto tableId = flat::ChainTableId(parser.get<uint32_t>("tableId"));
  auto csvFilePath = parser.get<std::string>("csv-file-path");
  auto desc = parser.present<String>("--desc").value_or("");

  if (parser.get<bool>("-d")) {
    std::ofstream of(csvFilePath);
    of.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    of << "ChainId\n";
    of << "123\n";
    of << "234\n";
    table.push_back({fmt::format("Dump template to {} succeeded", csvFilePath)});
    co_return table;
  }

  rapidcsv::Document doc(csvFilePath);
  auto columnNames = doc.GetColumnNames();

  if (columnNames.size() != 1) {
    co_return makeError(StatusCode::kInvalidFormat, "expected columns: ChainId");
  }

  if (columnNames[0] != "ChainId") {
    co_return makeError(StatusCode::kInvalidFormat,
                        fmt::format("column[0] expected:'ChainId' actual:'{}'", columnNames[0]));
  }

  if (doc.GetRowCount() == 0) {
    co_return makeError(StatusCode::kInvalidFormat, "empty rows");
  }

  std::vector<flat::ChainId> chainIds;
  for (size_t i = 0; i < doc.GetRowCount(); ++i) {
    auto row = doc.GetRow<int64_t>(i);
    if (row.size() != columnNames.size()) {
      co_return makeError(
          StatusCode::kInvalidFormat,
          fmt::format("unexpected size of row[{}]. expected:{} actual:{}", i, columnNames.size(), row.size()));
    }

    if (row[0] <= 0) {
      co_return makeError(StatusCode::kInvalidFormat, fmt::format("ChainId should be positive: {}", row[0]));
    }
    if (row[0] > static_cast<int64_t>(std::numeric_limits<flat::ChainId::UnderlyingType>::max())) {
      co_return makeError(StatusCode::kInvalidFormat,
                          fmt::format("ChainId overflow. max: {}. now: {}",
                                      std::numeric_limits<flat::ChainId::UnderlyingType>::max(),
                                      row[0]));
    }
    chainIds.emplace_back(row[0]);
  }

  auto rsp = co_await env.mgmtdClientGetter()->setChainTable(env.userInfo, tableId, chainIds, desc);
  CO_RETURN_ON_ERROR(rsp);

  table.push_back({fmt::format("Upload {} of {} succeeded", tableId, rsp->chainTableVersion)});
  co_return table;
}
}  // namespace
CoTryTask<void> registerUploadChainTableHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleUploadChainTable);
}
}  // namespace hf3fs::client::cli
