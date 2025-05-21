#!/bin/bash
source "$(dirname "$0")/_3fs_common.sh"

function run_monitor() {
    for var in CLICKHOUSE_DB CLICKHOUSE_HOST CLICKHOUSE_PASSWD CLICKHOUSE_PORT CLICKHOUSE_USER; do
        if [[ -z "${!var}" ]]; then
            echo "ERROR: Environment variable $var is not set"
            exit 1
        fi
    done

    if [[ ! -f "$CONFIG_DONE_FLAG" ]]; then
        # env: CLICKHOUSE_DB, CLICKHOUSE_HOST, CLICKHOUSE_PASSWD, CLICKHOUSE_PORT, CLICKHOUSE_USER, DEVICE_FILTER
        # monitor_collector_main.toml
        sed -i "/^\[server.monitor_collector.reporter.clickhouse\]/,/^\s*$/{
        s/db = '.*/db = '${CLICKHOUSE_DB}'/;
        s/host = '.*/host = '${CLICKHOUSE_HOST}'/;
        s/passwd = '.*/passwd = '${CLICKHOUSE_PASSWD}'/;
        s/port = '.*/port = '${CLICKHOUSE_PORT}'/;
        s/user = '.*/user = '${CLICKHOUSE_USER}'/;
        }" /opt/3fs/etc/monitor_collector_main.toml
        # device_filter if set
        if [[ -n "${DEVICE_FILTER}" ]]; then
            sed -i "s|device_filter = \[\]|device_filter = [\"${DEVICE_FILTER//,/\",\"}\"]|g" /opt/3fs/etc/monitor_collector_main.toml
        fi

        touch "$CONFIG_DONE_FLAG"
    fi
    # run monitor
    /opt/3fs/bin/monitor_collector_main --cfg /opt/3fs/etc/monitor_collector_main.toml
    # Prevent the main process from exiting, thereby avoiding container termination.
    tail -f /dev/null
}


run_monitor
