#!/bin/bash
source "$(dirname "$0")/_3fs_common.sh"

function run_storage() {
    for var in FDB_CLUSTER MGMTD_SERVER_ADDRESSES STORAGE_NODE_ID REMOTE_IP CLUSTER_ID; do
        if [[ -z "${!var}" ]]; then
            echo "ERROR: Environment variable $var is not set"
            exit 1
        fi
    done

    if [[ ! -f "$CONFIG_DONE_FLAG" ]]; then
        config_cluster_id
        # env: FDB_CLUSTER, MGMTD_SERVER_ADDRESSES, STORAGE_NODE_ID, TARGET_PATHS, DEVICE_FILTER, REMOTE_IP, CLUSTER_ID
        config_admin_cli
        # storage_main_launcher.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/storage_main_launcher.toml
        # storage_main_app.toml
        sed -i "s/^node_id.*/node_id = ${STORAGE_NODE_ID}/" /opt/3fs/etc/storage_main_app.toml
        # storage_main.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/storage_main.toml
        # /opt/3fs/etc/storage_main.toml
        if [[ -n "${TARGET_PATHS}" ]]; then
            sed -i "s|^target_paths = .*|target_paths = [\"${TARGET_PATHS//,/\",\"}\"]|g" /opt/3fs/etc/storage_main.toml
        fi
        sed -i "s|remote_ip = \".*\"|remote_ip = \"${REMOTE_IP}\"|g" /opt/3fs/etc/storage_main.toml
        # device_filter
        if [[ -n "${DEVICE_FILTER}" ]]; then
            sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/storage_main_launcher.toml
        fi
        # init storage
        /opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "set-config --type STORAGE --file /opt/3fs/etc/storage_main.toml"

        touch "$CONFIG_DONE_FLAG"
    fi
    # run storage
    /opt/3fs/bin/storage_main --launcher_cfg /opt/3fs/etc/storage_main_launcher.toml --app-cfg /opt/3fs/etc/storage_main_app.toml
    # Prevent the main process from exiting, thereby avoiding container termination.
    tail -f /dev/null
}


run_storage
