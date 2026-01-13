#!/bin/bash
CONFIG_DONE_FLAG="/opt/3fs/etc/.done"

function config_cluster_id() {
    # env: CLUSTER_ID
    sed -i "s/^cluster_id.*/cluster_id = \"${CLUSTER_ID:-default}\"/" /opt/3fs/etc/*
}


function config_admin_cli() {
    # env: FDB_CLUSTER, MGMTD_SERVER_ADDRESSES, DEVICE_FILTER, REMOTE_IP
    # admin_cli.toml
    echo ${FDB_CLUSTER} >/etc/foundationdb/fdb.cluster
    sed -i "s|^clusterFile.*|clusterFile = '/etc/foundationdb/fdb.cluster'|" /opt/3fs/etc/admin_cli.toml
    # device_filter
    if [[ -n "${DEVICE_FILTER}" ]]; then
        sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/admin_cli.toml
    fi
    sed -i "s|mgmtd_server_addresses = \[\]|mgmtd_server_addresses = [\"${MGMTD_SERVER_ADDRESSES//,/\",\"}\"]|g" /opt/3fs/etc/admin_cli.toml
}
