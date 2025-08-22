#!/bin/bash
set -euo pipefail

function cleanup {
    rv=$?
    jobs -p | xargs -r kill || true
    exit $rv
}
trap "cleanup" EXIT

if (( $# == 0 )) ; then
    echo "FDB_PATH=<fdb-binary-path> FDB_PORT=<fdb-port> FDB_CLUSTER_SIZE=<n> ${0} <data> <cmd>"
	exit 1
fi

DATA_DIR=${1}
COMMAND=${2:-}

echo $DATA_DIR $COMMAND

FDB_PORT=${FDB_PORT:-8000}
FDB_CLUSTER_SIZE=${FDB_CLUSTER_SIZE:-1}

echo $FDB_PORT $FDB_CLUSTER_SIZE

if [ -z ${FDB_PATH:-''} ]; then
    FDB=`which fdbserver`
    FDBCLI=`which fdbcli`
else
    FDB=$FDB_PATH/fdbserver
    FDBCLI=$FDB_PATH/fdbcli
fi

echo fdbserver: $FDB
echo fdbclient: $FDBCLI

rm -rf ${DATA_DIR}
mkdir -p ${DATA_DIR}

CLUSTER_FILE="test${FDB_PORT}:testdb${FDB_PORT}@127.0.0.1:${FDB_PORT}"
CLUSTER=${DATA_DIR}/fdb.cluster
echo $CLUSTER_FILE > $CLUSTER
echo "Starting Cluster: " $CLUSTER_FILE

for j in `seq 1 ${FDB_CLUSTER_SIZE}`; do
    PORT=`expr $FDB_PORT + $j - 1`
    LOG=${DATA_DIR}/${j}/log
    DATA=${DATA_DIR}/${j}/data
    mkdir -p ${LOG} ${DATA}
    ${FDB} -p auto:${PORT}`` -d $DATA -L $LOG -C $CLUSTER &
done

CLI="$FDBCLI -C ${CLUSTER} --exec"
sleep 5 && $CLI "configure new memory single"
sleep 5 && echo "Check Foundationdb status:" `$CLI status`

echo ""
echo "Run test:"
FDB_CLUSTER_FILE=`realpath ${CLUSTER}`
echo export FDB_UNITTEST_CLUSTER=${FDB_CLUSTER_FILE}
export FDB_UNITTEST_CLUSTER=$FDB_CLUSTER_FILE

if [ -z ${COMMAND} ]; then
    while read -p "> " -r command; do
        echo Executing command: $command
        eval $command
    done
else
    echo Executing command: ${COMMAND}
    eval ${COMMAND}
fi
