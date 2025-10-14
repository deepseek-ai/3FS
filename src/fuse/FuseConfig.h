#pragma once

#include "client/meta/MetaClient.h"
#include "client/mgmtd/MgmtdClientForClient.h"
#include "client/storage/StorageClient.h"
#include "common/app/ApplicationBase.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/CoroutinesPool.h"

namespace hf3fs::fuse {
struct FuseConfig : public ConfigBase<FuseConfig> {
#ifdef ENABLE_FUSE_APPLICATION
  CONFIG_OBJ(common, ApplicationBase::Config);
#else
  CONFIG_ITEM(cluster_id, "");
  CONFIG_ITEM(token_file, "");
  CONFIG_ITEM(mountpoint, "");
  CONFIG_ITEM(allow_other, true);
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  CONFIG_OBJ(log, logging::LogConfig);
  CONFIG_OBJ(monitor, monitor::Monitor::Config);
#endif
  CONFIG_HOT_UPDATED_ITEM(enable_priority, false);
  CONFIG_HOT_UPDATED_ITEM(enable_interrupt, false);
  CONFIG_HOT_UPDATED_ITEM(attr_timeout, (double)30);
  CONFIG_HOT_UPDATED_ITEM(entry_timeout, (double)30);
  CONFIG_HOT_UPDATED_ITEM(negative_timeout, (double)5);
  CONFIG_HOT_UPDATED_ITEM(symlink_timeout, (double)5);
  CONFIG_HOT_UPDATED_ITEM(readonly, false);
  CONFIG_HOT_UPDATED_ITEM(memset_before_read, false);
  CONFIG_HOT_UPDATED_ITEM(enable_read_cache, true);
  CONFIG_HOT_UPDATED_ITEM(fsync_length_hint, false);  // for test
  CONFIG_HOT_UPDATED_ITEM(fdatasync_update_length, false);
  CONFIG_ITEM(max_idle_threads, -1);
  CONFIG_ITEM(max_threads, 256);
  CONFIG_ITEM(max_readahead, 16_MB);
  CONFIG_ITEM(max_background, 32);
  CONFIG_ITEM(enable_writeback_cache, false);
  CONFIG_OBJ(client, net::Client::Config);
  CONFIG_OBJ(mgmtd, client::MgmtdClientForClient::Config);
  CONFIG_OBJ(storage, storage::client::StorageClient::Config);
  CONFIG_OBJ(meta, meta::client::MetaClient::Config, [&](auto &cfg) { cfg.set_dynamic_stripe(true); });
  CONFIG_ITEM(remount_prefix, (std::optional<std::string>)std::nullopt);
  CONFIG_ITEM(iov_limit, 1_MB);
  CONFIG_ITEM(io_jobq_size, 1024);
  CONFIG_ITEM(batch_io_coros, 128);
  CONFIG_ITEM(rdma_buf_pool_size, 1024);
  CONFIG_ITEM(time_granularity, 1_s);
  CONFIG_HOT_UPDATED_ITEM(check_rmrf, true);
  CONFIG_ITEM(notify_inval_threads, 32);

  CONFIG_ITEM(max_uid, 1_M);

  CONFIG_HOT_UPDATED_ITEM(chunk_size_limit, 0_KB);

  CONFIG_SECT(io_jobq_sizes, {
    CONFIG_ITEM(hi, 32);
    CONFIG_ITEM(lo, 4096);
  });

  CONFIG_SECT(io_worker_coros, {
    CONFIG_HOT_UPDATED_ITEM(hi, 8);
    CONFIG_HOT_UPDATED_ITEM(lo, 8);
  });

  CONFIG_HOT_UPDATED_ITEM(io_job_deq_timeout, 1_ms);

  CONFIG_OBJ(storage_io, storage::client::IoOptions);

  CONFIG_HOT_UPDATED_ITEM(submit_wait_jitter, 1_ms);
  CONFIG_HOT_UPDATED_ITEM(max_jobs_per_ioring, 32);

  CONFIG_SECT(io_bufs, {
    CONFIG_ITEM(max_buf_size, 1_MB);
    CONFIG_ITEM(max_readahead, 256_KB);
    CONFIG_ITEM(write_buf_size, 1_MB);
  });

  CONFIG_HOT_UPDATED_ITEM(flush_on_stat, true);
  CONFIG_HOT_UPDATED_ITEM(sync_on_stat, true);
  CONFIG_HOT_UPDATED_ITEM(dryrun_bench_mode, false);

  struct PeriodSync : public ConfigBase<PeriodSync> {
    CONFIG_HOT_UPDATED_ITEM(enable, true);
    CONFIG_HOT_UPDATED_ITEM(interval, 30_s);
    CONFIG_HOT_UPDATED_ITEM(limit, 1000u);
    CONFIG_HOT_UPDATED_ITEM(flush_write_buf, true);
    CONFIG_OBJ(worker, CoroutinesPoolBase::Config, [](auto &cfg) { cfg.set_coroutines_num(4); });
  };
  CONFIG_OBJ(periodic_sync, PeriodSync);
};
}  // namespace hf3fs::fuse
