#pragma once

#include "common/kv/TransactionRetry.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "core/user/UserCache.h"

namespace hf3fs::mgmtd {
struct MgmtdConfig : ConfigBase<MgmtdConfig> {
  CONFIG_HOT_UPDATED_ITEM(lease_length, 60_s);
  CONFIG_HOT_UPDATED_ITEM(extend_lease_interval, 10_s);
  CONFIG_HOT_UPDATED_ITEM(suspicious_lease_interval, 20_s);
  CONFIG_HOT_UPDATED_ITEM(heartbeat_timestamp_valid_window, 30_s);
  CONFIG_HOT_UPDATED_ITEM(allow_heartbeat_from_unregistered, false);
  CONFIG_HOT_UPDATED_ITEM(check_status_interval, 10_s);
  CONFIG_HOT_UPDATED_ITEM(heartbeat_fail_interval, 60_s);
  CONFIG_HOT_UPDATED_ITEM(new_chain_bootstrap_interval, 2_min);
  CONFIG_HOT_UPDATED_ITEM(send_heartbeat, true);
  CONFIG_HOT_UPDATED_ITEM(send_heartbeat_interval, 10_s);
  CONFIG_HOT_UPDATED_ITEM(client_session_timeout, 20_min);
  CONFIG_HOT_UPDATED_ITEM(bootstrapping_length, 2_min);
  CONFIG_HOT_UPDATED_ITEM(update_chains_interval, 1_s);
  CONFIG_HOT_UPDATED_ITEM(validate_lease_on_write, true);
  CONFIG_HOT_UPDATED_ITEM(bump_routing_info_version_interval, 5_s);
  // deprecated
  CONFIG_HOT_UPDATED_ITEM(heartbeat_ignore_unknown_targets, false);
  CONFIG_HOT_UPDATED_ITEM(heartbeat_ignore_stale_targets, true);
  CONFIG_HOT_UPDATED_ITEM(retry_times_on_txn_errors, -1);  // -1 means infitely retry, 0 means no retry
  CONFIG_HOT_UPDATED_ITEM(update_metrics_interval, 1_s);
  CONFIG_HOT_UPDATED_ITEM(target_info_persist_interval, 1_s);
  CONFIG_HOT_UPDATED_ITEM(target_info_persist_batch, 1000);
  CONFIG_HOT_UPDATED_ITEM(target_info_load_interval, 1_s);
  CONFIG_HOT_UPDATED_ITEM(try_adjust_target_order_as_preferred, false);
  CONFIG_HOT_UPDATED_ITEM(extend_lease_check_release_version, true);
  CONFIG_HOT_UPDATED_ITEM(authenticate, false);
  CONFIG_OBJ(retry_transaction, kv::TransactionRetry);
  CONFIG_OBJ(user_cache, core::UserCache::Config);
  CONFIG_HOT_UPDATED_ITEM(enable_routinginfo_cache, true);
  CONFIG_HOT_UPDATED_ITEM(only_accept_client_uuid, false);
};
}  // namespace hf3fs::mgmtd
