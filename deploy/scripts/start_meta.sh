#!/bin/bash
source "$(dirname "$0")/_3fs_common.sh"

function run_meta() {
    for var in FDB_CLUSTER MGMTD_SERVER_ADDRESSES META_NODE_ID REMOTE_IP CLUSTER_ID; do
        if [[ -z "${!var}" ]]; then
            echo "ERROR: Environment variable $var is not set"
            exit 1
        fi
    done

    if [[ ! -f "$CONFIG_DONE_FLAG" ]]; then
        config_cluster_id
        # env: FDB_CLUSTER, MGMTD_SERVER_ADDRESSES, META_NODE_ID, DEVICE_FILTER, REMOTE_IP, CLUSTER_ID
        config_admin_cli
        # meta_main_launcher.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/meta_main_launcher.toml
        # meta_main_app.toml
        sed -i "s/^node_id.*/node_id = ${META_NODE_ID}/" /opt/3fs/etc/meta_main_app.toml
        # meta_main.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/meta_main.toml
        sed -i "s|remote_ip = .*|remote_ip = '${REMOTE_IP}'|g" /opt/3fs/etc/meta_main.toml
        # device_filter
        if [[ -n "${DEVICE_FILTER}" ]]; then
            sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/meta_main_launcher.toml
        fi
        # init meta
        /opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "set-config --type META --file /opt/3fs/etc/meta_main.toml"

        touch "$CONFIG_DONE_FLAG"
    fi
    # run meta
    /opt/3fs/bin/meta_main --launcher_cfg /opt/3fs/etc/meta_main_launcher.toml --app-cfg /opt/3fs/etc/meta_main_app.toml
    # Prevent the main process from exiting, thereby avoiding container termination.
    tail -f /dev/null
}


run_meta
