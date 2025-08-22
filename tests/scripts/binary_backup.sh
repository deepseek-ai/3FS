#/bin/bash

set -e

# This scripe is used by CI to back test binarys when failure.

if (( $# != 3 )) ; then
	echo "Usage 'cd working-directory; ${0} <build-directory> <backup-directory> <pipeline-id>' "
	exit 1
fi

echo 'Back up binarys and logs.'

BUILD_DIRECTORY=${1}
BACKUP_DIRECTORY=${2}
PIPELINE_ID=${3}

mkdir -p ${BACKUP_DIRECTORY}

# To avoid backup directory use up space, clean backup directory before test.
# Only keeps backup files created in 72 hours.
OLD_BACKUPS=`find ${BACKUP_DIRECTORY} -type f -mtime +3 -name 'backup-*.tar.gz'`
echo 'Remove old updates:' ${OLD_BACKUPS}
echo ${OLD_BACKUPS} | xargs rm -f

# Save binarys.
mkdir -p ${BUILD_DIRECTORY}/backup
cp ${BUILD_DIRECTORY}/tests/test_* ${BUILD_DIRECTORY}/backup || true

BACKUP_PATH=${BACKUP_DIRECTORY}/backup-${PIPELINE_ID}.tar.gz
echo 'Tar all files to' ${BACKUP_PATH}
tar -zcvf ${BACKUP_PATH} ${BUILD_DIRECTORY}/backup
