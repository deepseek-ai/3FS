#!/bin/bash
source "$(dirname "$0")/_3fs_common.sh"

function run_fuse() {
    for var in FDB_CLUSTER MGMTD_SERVER_ADDRESSES TOKEN REMOTE_IP CLUSTER_ID; do
        if [[ -z "${!var}" ]]; then
            echo "ERROR: Environment variable $var is not set"
            exit 1
        fi
    done

    if [[ ! -f "$CONFIG_DONE_FLAG" ]]; then
        config_cluster_id
        # env: FDB_CLUSTER, MGMTD_SERVER_ADDRESSES, TOKEN, DEVICE_FILTER, REMOTE_IP
        config_admin_cli
        mkdir -p  /3fs/stage
        # TOKEN
        echo ${TOKEN} >/opt/3fs/etc/token.txt
        # hf3fs_fuse_main_launcher.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/hf3fs_fuse_main_launcher.toml
        sed -i "s|mountpoint = ''|mountpoint = '/3fs/stage'|g" /opt/3fs/etc/hf3fs_fuse_main_launcher.toml
        sed -i "s|token_file = ''|token_file = '/opt/3fs/etc/token.txt'|g" /opt/3fs/etc/hf3fs_fuse_main_launcher.toml
        # hf3fs_fuse_main.toml
        sed -i "s|remote_ip = ''|remote_ip = \"${REMOTE_IP}\"|g" /opt/3fs/etc/hf3fs_fuse_main.toml
        sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/hf3fs_fuse_main.toml
        # device_filter
        if [[ -n "${DEVICE_FILTER}" ]]; then
            sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/hf3fs_fuse_main_launcher.toml
        fi
        # init fuse
        /opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "set-config --type FUSE --file /opt/3fs/etc/hf3fs_fuse_main.toml"

        touch "$CONFIG_DONE_FLAG"
    fi
    # run fuse
    /opt/3fs/bin/hf3fs_fuse_main --launcher_cfg /opt/3fs/etc/hf3fs_fuse_main_launcher.toml

    # Prevent the main process from exiting, thereby avoiding container termination.
    tail -f /dev/null
}


run_fuse
