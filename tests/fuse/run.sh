#!/bin/bash

# ======== Helper Functions ========

function check_exists {
    if ! [ -e "$1" ]; then 
        echo "$1 not exists"
        exit 1
    fi
}

function start_fdb {
    local PORT="$1"
    local DATA="$2"
    local LOG="$3"
    local CLUSTER="${DATA}/fdb.cluster"

    echo "fdbserver: $FDB, fdbclient: $FDBCLI, start fdb @ port ${PORT}"
    echo "test${FDB_PORT}:testdb${FDB_PORT}@127.0.0.1:${FDB_PORT}" > "${CLUSTER}"
    "${FDB}" -p auto:"${PORT}" -d "${DATA}" -L "${LOG}" -C "${CLUSTER}" &> "${LOG}/fdbserver.log" &
    sleep 5 && "$FDBCLI" -C "${CLUSTER}" --exec "configure new memory single"
    sleep 5 && "$FDBCLI" -C "${CLUSTER}" --exec "status minimal"
}

function cleanup {
    rv=$?
    echo "Exit with ${rv}"
    fusermount3 -u "${MOUNT}" || true
    sleep 1
    jobs -p | xargs -r kill || true
    sleep 1
    jobs -p | xargs -r kill -9 &>/dev/null || true
    fusermount3 -u "${MOUNT}" &>/dev/null || true
    exit $rv
}

# ======== Main Script ========

# Parameter validation
if (($# < 2)); then
    echo "${0} <binary> <test-dir> <log-dir>"
    exit 1
fi

# Set variables
BINARY=${1%/}
TEST_DIR=${2%/}
LOG_DIR=${3:-}
mkdir -p ${TEST_DIR}

# Set exit trap and error handling
trap "cleanup" EXIT
set -euo pipefail

# Define paths
DATA="${TEST_DIR}/data"
CONFIG="${TEST_DIR}/config"
DEFAULT_LOG="${TEST_DIR}/log"
LOG=${LOG_DIR:=${DEFAULT_LOG}}
MOUNT=$(realpath "${TEST_DIR}/mnt")
export MGMTD_LOG=${LOG}/mgmtd.log
export META_LOG=${LOG}/meta.log
export STORAGE_LOG=${LOG}/storage.log
export FUSE_LOG=${LOG}/fuse.log

# Script related tools
SCRIPT_DIR=$(dirname "$0")
UPDATE_CONFIG="python3 ${SCRIPT_DIR}/update-config.py"

# Set environment variables
export TOKEN="AADHHSOs8QA92iRe2wB1fmuL"
export TOKEN_FILE="${CONFIG}/token"

export PKEY_INDEX=0
export FS_CLUSTER="ci_test"
export FDB_TEST_CLUSTER="${DATA}/foundationdb/fdb.cluster"
export TARGETS="'${DATA}/storage/data1', '${DATA}/storage/data2', '${DATA}/storage/data3', '${DATA}/storage/data4'"

# Set FoundationDB paths
if [ -z "${FDB_PATH+x}" ]; then
    FDB=$(which fdbserver)
    FDBCLI=$(which fdbcli)
else
    FDB="$FDB_PATH/fdbserver"
    FDBCLI="$FDB_PATH/fdbcli"
fi

# ======== Network Configuration ========
echo "### Check network"
export ADDRESS=$(ip -o -4 addr show | grep -E '(en|eth|ib)' | awk '{print $4}' | head -n 1 | awk -F'[/ ]' '{print $1}')
export FDB_PORT=12500
export MGMTD_PORT=12501
export META_PORT=12502
export STORAGE_PORT=12503

# ======== Check Directories and Files ========
echo "### Check directory and file"
check_exists "${TEST_DIR}"
if [ -z "$(ls -A ${TEST_DIR})" ]; then
    echo ${TEST_DIR} is empty
else
    echo ${TEST_DIR} is not empty
    exit 1
fi
mkdir -p ${DATA}/storage/data{1..4}

check_exists "${BINARY}"
check_exists "${BINARY}/mgmtd_main"
check_exists "${BINARY}/meta_main"
check_exists "${BINARY}/storage_main"
check_exists "${BINARY}/hf3fs_fuse_main"
check_exists "${BINARY}/admin_cli"
check_exists "${FDB}"
check_exists "${FDBCLI}"

# ======== Create Directories ========
echo "### Create subdirectories"
mkdir -p "${CONFIG}" "${LOG}" "${DATA}" "${MOUNT}" "${DATA}/foundationdb" "${DATA}/storage"

# ======== Generate Configuration Files ========
echo "Generate config"
# mgmtd app configuration
echo "allow_empty_node_id = false" > "${CONFIG}/mgmtd_main_app.toml"
echo "node_id = 1" >> "${CONFIG}/mgmtd_main_app.toml"
# meta app configuration
echo "allow_empty_node_id = false" > "${CONFIG}/meta_main_app.toml"
echo "node_id = 50" >> "${CONFIG}/meta_main_app.toml"
# storage app configuration
echo "allow_empty_node_id = false" > "${CONFIG}/storage_main_app.toml"
echo "node_id = 10000" >> "${CONFIG}/storage_main_app.toml"
echo ${TOKEN} > ${TOKEN_FILE}

# Copy and process template configurations
for file in "${SCRIPT_DIR}"/config/*.toml; do
    echo "- Generate $(basename "$file")"
    cat "${file}" | envsubst > "${CONFIG}/$(basename "$file")"
done
ADMIN_CLI="${BINARY}/admin_cli --cfg ${CONFIG}/admin_cli.toml --"

# ======== Create FoundationDB ========
echo "### Create foundationdb"
start_fdb $FDB_PORT "${DATA}/foundationdb" "${LOG}/"

# ======== Initialize Cluster ========
echo "### Init cluster"
${ADMIN_CLI} user-add --root --admin --token "${TOKEN}" 0 root
${ADMIN_CLI} user-set-token --new 0
${ADMIN_CLI} user-list
${ADMIN_CLI} init-cluster \
    --mgmtd "${CONFIG}/mgmtd_main.toml" \
    --meta "${CONFIG}/meta_main.toml" \
    --storage "${CONFIG}/storage_main.toml" \
    --fuse "${CONFIG}/hf3fs_fuse_main.toml" \
    --skip-config-check 1 524288 1

# ======== Start Services ========
echo "### Start mgmtd"
"${BINARY}/mgmtd_main" \
    --app_cfg "${CONFIG}/mgmtd_main_app.toml" \
    --launcher_cfg "${CONFIG}/mgmtd_main_launcher.toml" \
    --cfg "${CONFIG}/mgmtd_main.toml" \
    > "${LOG}/mgmtd_main.stdout" 2> "${LOG}/mgmtd_main.stderr" &
sleep 5

echo "### Start meta & storage"
"${BINARY}/storage_main" \
    --app_cfg "${CONFIG}/storage_main_app.toml" \
    --launcher_cfg "${CONFIG}/storage_main_launcher.toml" \
    --cfg "${CONFIG}/storage_main.toml" \
    > "${LOG}/storage_main.stdout" 2> "${LOG}/storage_main.stderr" &

"${BINARY}/meta_main" \
    --app_cfg "${CONFIG}/meta_main_app.toml" \
    --launcher_cfg "${CONFIG}/meta_main_launcher.toml" \
    --cfg "${CONFIG}/meta_main.toml" \
    > "${LOG}/meta_main.stdout" 2> "${LOG}/meta_main.stderr" &

sleep 15

# ======== Configure Nodes and Chains ========
echo "### List nodes"
${ADMIN_CLI} list-nodes

echo "### Create targets and chain table"
echo "ChainId,TargetId" > "${CONFIG}/chains.csv"
echo "ChainId" > "${CONFIG}/chain-table.csv"
for disk in $(seq 1 4); do
    target=$(printf "%d%02d001" 10000 $disk)
    ${ADMIN_CLI} create-target --node-id 10000 --disk-index $((disk - 1)) --target-id "${target}" --chain-id "${target}"
    echo "${target},${target}" >> "${CONFIG}/chains.csv"
    echo "${target}" >> "${CONFIG}/chain-table.csv"
done

${ADMIN_CLI} upload-chains "${CONFIG}/chains.csv"
${ADMIN_CLI} upload-chain-table 1 "${CONFIG}/chain-table.csv" --desc replica-1
sleep 10
${ADMIN_CLI} list-chains
${ADMIN_CLI} list-chain-tables

if [ $UID -ne 0 ]; then ${ADMIN_CLI} user-add ${UID} test; fi
${ADMIN_CLI} user-list
${ADMIN_CLI} mkdir --perm 0755 test
${ADMIN_CLI} set-perm --uid ${UID} --gid ${UID} test

# ======== Start FUSE ========
echo "### Start fuse"
"${BINARY}/hf3fs_fuse_main" \
    --launcher_cfg "${CONFIG}/hf3fs_fuse_main_launcher.toml" \
    --launcher_config.mountpoint="${MOUNT}" \
    > "${LOG}/fuse.stdout" 2> "${LOG}/fuse.stderr" &

# Wait for mount point to be ready
SECONDS=0
while [ ! -d "${MOUNT}/test" ] && [ ${SECONDS} -lt 30 ]; do
    sleep 1
done
if [ ! -d "${MOUNT}/test" ]; then
    echo "Mount point not ready within 30 seconds"
    exit 1
fi

echo "A temporary 3FS has been mounted to ${MOUNT}"
echo "This file system is for testing purposes only."
echo "Do not store any important data or you will lose them."
sleep infinity || true

echo "Unmount fuse and stop all servers"
fusermount3 -u "${MOUNT}"