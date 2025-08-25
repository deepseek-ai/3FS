#!/bin/bash
source "$(dirname "$0")/_3fs_common.sh"

function run_mgmtd() {
    for var in FDB_CLUSTER MGMTD_SERVER_ADDRESSES MGMTD_NODE_ID REMOTE_IP CLUSTER_ID; do
        if [[ -z "${!var}" ]]; then
            echo "ERROR: Environment variable $var is not set"
            exit 1
        fi
    done

    if [[ ! -f "$CONFIG_DONE_FLAG" ]]; then    
        config_cluster_id
        # env: FDB_CLUSTER, MGMTD_SERVER_ADDRESSES, MGMTD_NODE_ID, DEVICE_FILTER, REMOTE_IP, CLUSTER_ID
        config_admin_cli
        echo ${FDB_CLUSTER} >/etc/foundationdb/fdb.cluster
        # mgmtd_main_launcher.toml
        sed -i "/\[fdb\]/,/^\[/{s|^clusterFile.*|clusterFile = '/etc/foundationdb/fdb.cluster'|}" /opt/3fs/etc/mgmtd_main_launcher.toml
        # mgmtd_main_app.toml
        sed -i "s/^node_id.*/node_id = ${MGMTD_NODE_ID}/" /opt/3fs/etc/mgmtd_main_app.toml
        # mgmtd_main.toml
        sed -i "s|remote_ip = .*|remote_ip = '${REMOTE_IP}'|g" /opt/3fs/etc/mgmtd_main.toml
        # device_filter
        if [[ -n "${DEVICE_FILTER}" ]]; then
            sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/mgmtd_main_launcher.toml
        fi

        if [[ -z "${STRIP_SIZE}" ]]; then
           STRIP_SIZE=6 
        fi

        /opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "init-cluster --mgmtd /opt/3fs/etc/mgmtd_main.toml 1 1048576 $STRIP_SIZE"
        if [ $? -ne 0 ]; then
            echo "ERROR: init-cluster failed"
            exit 1
        fi

        touch "$CONFIG_DONE_FLAG"
    fi
    # run mgmtd
    /opt/3fs/bin/mgmtd_main --launcher_cfg /opt/3fs/etc/mgmtd_main_launcher.toml --app-cfg /opt/3fs/etc/mgmtd_main_app.toml
    # Prevent the main process from exiting, thereby avoiding container termination.
    tail -f /dev/null
}


run_mgmtd
