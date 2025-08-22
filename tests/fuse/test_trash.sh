#!/bin/bash

RED='\033[0;31m'
set -e

error_handler() {
    echo "Error occurred on line $1"
}

# 设置 trap 来捕获 ERR 信号
trap 'error_handler $LINENO' ERR

check_exists() {
    local path="$1"
    echo check $path
    if [ -e "$path" ]; then
        echo "ok, 路径存在: $path"
    else
        echo "${RED}error, 路径不存在: $path"
        exit 1
    fi
}

# 检查路径是否不存在
check_not_exists() {
    local path="$1"
    if [ ! -e "$path" ]; then
        echo "ok, 路径不存在: $path"
    else
        echo "${RED}error, 路径存在: $path"
        exit 1
    fi
}

assert_fail() {
    set +e

    # 执行传入的命令（包括管道）
    eval "$@"

    # 检查整个管道的退出状态码
    if [ $? -eq 0 ]; then
        echo "Assertion failed: Command '$*' succeeded, but expected to fail."
        exit 1
    else
        echo "Assertion passed: Command '$*' failed as expected."
    fi

    set -e
}

if (($# < 2)); then
    echo "${0} <binary> <test-dir> <log-dir>"
    exit 1
fi

BINARY=${1%/}
TEST_DIR=${2%/}
LOG_DIR=${3:-}
DEFAULT_LOG=${TEST_DIR}/log
LOG=${LOG_DIR:=${DEFAULT_LOG}}

MOUNT1=${TEST_DIR}/mnt1
MOUNT2=${TEST_DIR}/mnt2
export HF3FS_CLI_MOUNTPOINT=`realpath ${MOUNT1}`

# test hf3fs trash
sudo rm -rf ${MOUNT1}/trash
# without trash directory, can't rmtree
mkdir -p ${MOUNT1}/test/subdir
assert_fail 'yes | hf3fs_cli rmtree --expire 3hours ${MOUNT1}/test/subdir'

sudo mkdir ${MOUNT1}/trash
sudo mkdir -p ${MOUNT1}/trash/`id -un`
sudo chown ${UID}:${UID} ${MOUNT1}/trash/`id -un`
mkdir -p ${MOUNT1}/trash/`id -un`/invalid-name
mkdir -p ${MOUNT1}/trash/`id -un`/1d-20240801_1200-20240801_1000 # invalid timestamp, begin < end
mkdir -p ${MOUNT1}/trash/`id -un`/1d-20240801_1200-20240801_1300 # expired
mkdir -p ${MOUNT1}/trash/`id -un`/1d-20240801_1200-20240801_1300/data-{1..200}
mkdir -p ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300 # not expired
mkdir -p ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300/data-{1..200}
# trash cleaner 在删除时使用的是普通用户的 uid/gid, 不应该有 root 权限
sudo mkdir -p ${MOUNT1}/trash/`id -un`/noperm-20240801_1200-20240801_1300/data-{1..20}

# root user's directory
sudo mkdir ${MOUNT1}/trash/user-root
sudo mkdir -p ${MOUNT1}/trash/user-root/1d-20240801_1200-20240801_1300/data
# another user's directory
sudo mkdir ${MOUNT1}/trash/user-1000
sudo chown 1000:1000 ${MOUNT1}/trash/user-1000
sudo mkdir -p ${MOUNT1}/trash/user-1000/1d-20240801_1200-20240801_1300/data
sudo mkdir -p ${MOUNT1}/trash/user-1000/1d-20240801_1200-20240801_1310/data
sudo chown -R 1000:1000 ${MOUNT1}/trash/user-1000/1d-20240801_1200-20240801_1310

sudo ${BINARY}/trash_cleaner --interval 0 --paths ${MOUNT1}/trash/

# after clean check some directory is cleaned, some still exists
echo ==== ${MOUNT1}/trash/`id -un` ====
ls ${MOUNT1}/trash/`id -un`
check_exists ${MOUNT1}/trash/`id -un`/invalid-name
check_exists ${MOUNT1}/trash/`id -un`/1d-20240801_1200-20240801_1000
check_exists ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300
check_not_exists ${MOUNT1}/trash/`id -un`/1d-20240801_1200-20240801_1300
check_exists ${MOUNT1}/trash/`id -un`/noperm-20240801_1200-20240801_1300/data-{1..20}

echo ==== ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300 ====
check_exists ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300
ls ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300
# rmtree to remove trash immediately
assert_fail 'yes | hf3fs_cli rmtree ${MOUNT1}/trash/`id -un`/'
assert_fail 'yes | hf3fs_cli rmtree ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300 --expire 3hours'
check_exists ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300
yes | hf3fs_cli rmtree ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300
check_not_exists ${MOUNT1}/trash/`id -un`/1d-20300801_1200-20300801_1300

# root user's trash directory should exists
check_exists ${MOUNT1}/trash/user-root
check_exists ${MOUNT1}/trash/user-root/1d-20240801_1200-20240801_1300/data

# another user's trash directory
check_exists ${MOUNT1}/trash/user-1000/
# no permission to remove
check_exists ${MOUNT1}/trash/user-1000/1d-20240801_1200-20240801_1300/data
check_not_exists ${MOUNT1}/trash/user-1000/1d-20240801_1200-20240801_1310/data

# test hf3fs cli
sudo rm -rf ${MOUNT1}/root
sudo mkdir ${MOUNT1}/root
sudo mkdir ${MOUNT1}/root/`id -un`
sudo chown ${UID}:${UID} ${MOUNT1}/root/`id -un`
mkdir ${MOUNT1}/root/`id -un`/subdir

assert_fail 'yes | hf3fs_cli rmtree --expire 3hours ${MOUNT1}/test/some-not-found-path'
assert_fail 'hf3fs_cli mv ${MOUNT1}/test/some-not-found-path ${MOUNT1}/'
assert_fail 'hf3fs_cli mv ${MOUNT1}/root/`id -un`/subdir ${MOUNT1}/root/`id -un`/subdir'

# don't support ..
assert_fail 'yes | hf3fs_cli rmtree --expire 1h ${MOUNT1}/root/../root/`id -un`/subdir'
assert_fail 'yes | hf3fs_cli rmtree --expire 1h ${MOUNT1}/root/`id -un`/subdir2/../subdir'

hf3fs_cli mv ${MOUNT1}/root/`id -un`/subdir ${MOUNT1}/root/`id -un`/another-dir
assert_fail 'hf3fs_cli mv ${MOUNT1}/root/`id -un`/another-dir ${MOUNT1}/root/`id -un`/another-dir/another-dir'
mkdir ${MOUNT1}/root/`id -un`/subdir
hf3fs_cli mv ${MOUNT1}/root/`id -un`/subdir ${MOUNT1}/root/`id -un`/another-dir
check_exists ${MOUNT1}/root/`id -un`/another-dir/subdir
mkdir ${MOUNT1}/root/`id -un`/subdir
assert_fail 'hf3fs_cli mv ${MOUNT1}/root/`id -un`/subdir ${MOUNT1}/root/`id -un`/another-dir'

yes | hf3fs_cli rmtree --expire 3hours ${MOUNT1}/root/`id -un`/another-dir
mkdir ${MOUNT1}/root/`id -un`/another-dir
yes | hf3fs_cli rmtree --expire 3hours ${MOUNT1}/root/`id -un`/another-dir
mkdir ${MOUNT1}/root/`id -un`/another-dir
yes | hf3fs_cli rmtree --expire 3hours ${MOUNT1}/root/`id -un`/another-dir
stat ${MOUNT1}/trash/`id -un`/*/another-dir
stat ${MOUNT1}/trash/`id -un`/*/another-dir.*

#don't allow move into trash
mkdir ${MOUNT1}/root/`id -un`/adir
assert_fail 'mv ${MOUNT1}/root/`id -un`/adir ${MOUNT1}/trash/`id -un`/'
# allow mv between trash
mkdir ${MOUNT1}/trash/`id -un`/srctrash
mv ${MOUNT1}/trash/`id -un`/srctrash ${MOUNT1}/trash/`id -un`/dsttrash

# rmtree don't support symlink
pushd ${MOUNT1}/root/`id -un`/
mkdir -p dir1/subdir
ln -s dir1 symlink-to-dir1
yes | hf3fs_cli rmtree --expire 3hours symlink-to-dir1
check_not_exists symlink-to-dir1
check_exists dir1
yes | hf3fs_cli rmtree --expire 3hours dir1
check_not_exists dir1

mkdir -p dir-otheruser/subdir
sudo chown -R 999:999 dir-otheruser
sudo chmod 0777 dir-otheruser
assert_fail 'yes | hf3fs_cli rmtree --expire 3hours dir-otheruser'
check_exists dir-otheruser
popd

# don't allow cross filesystem link
mkdir ${MOUNT1}/root/`id -un`/src
assert_fail 'hf3fs_cli mv ${MOUNT1}/root/`id -un`/src ${MOUNT2}/root/`id -un`/dest'
hf3fs_cli mv ${MOUNT1}/root/`id -un`/src ${MOUNT1}/root/`id -un`/dest
echo ======= 测试完成 ========