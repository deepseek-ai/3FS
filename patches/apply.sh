#!/bin/bash

set -e

cd "$(dirname "$0")"

if git -C ../third_party/rocksdb apply --reverse --check ../../patches/rocksdb.patch &>/dev/null; then
    echo "rocksdb patch already applied. skipping."
else
    git -C ../third_party/rocksdb apply ../../patches/rocksdb.patch
fi

if git -C ../third_party/rocksdb apply --reverse --check ../../patches/rocksdb.patch.2 &>/dev/null; then
    echo "rocksdb patch 2 already applied. skipping."
else
    git -C ../third_party/rocksdb apply ../../patches/rocksdb.patch.2
fi

if git -C ../third_party/folly apply --reverse --check ../../patches/folly.patch &>/dev/null; then
    echo "folly patch already applied. skipping."
else
    git -C ../third_party/folly apply ../../patches/folly.patch
fi
