#include "Layout.h"

DEFINE_int32(chain_table_id, 0, "Chain table id for the file layout");
DEFINE_int32(chain_table_ver, 0, "Chain table for the file layout");
DEFINE_string(chunk_size, "0", "Chunk size for the file layout");
DEFINE_int32(stripe_size, 0, "Stripe size for the file layout");
DEFINE_string(chain_index_list, "", "List of chain indexList for the file layout");

namespace hf3fs::tools {
meta::Layout layoutFromFlags() {
  auto chainTable = flat::ChainTableId(FLAGS_chain_table_id);
  auto chainTableVersion = flat::ChainTableVersion(FLAGS_chain_table_ver);
  auto cs = Size::from(FLAGS_chunk_size);
  XLOGF_IF(FATAL, !cs, "invalid chunk size {}", FLAGS_chunk_size);
  auto chunkSize = (uint64_t)*cs;
  if (FLAGS_chain_index_list.empty()) {
    return meta::Layout::newEmpty(chainTable, chunkSize, FLAGS_stripe_size);
  } else {
    std::vector<std::string> idxs;
    folly::split(',', FLAGS_chain_index_list, idxs, true);
    std::vector<uint32_t> chains;
    chains.reserve(idxs.size());
    for (auto &&idx : idxs) {
      char *p;
      auto cidx = strtoul(idx.c_str(), &p, 10);
      XLOGF_IF(FATAL, !cidx && *p, "invalid chain index {}, the whole list {}", cidx, FLAGS_chain_index_list);
      XLOGF(DBG, "chain index without table id {}", cidx);
      chains.push_back(cidx);
    }

    for (auto cid : chains) {
      XLOGF(DBG, "chain indexList {}", cid);
    }

    return meta::Layout::newChainList(chainTable, chainTableVersion, chunkSize, chains);
  }
}
}  // namespace hf3fs::tools
