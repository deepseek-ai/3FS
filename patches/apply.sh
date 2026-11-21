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

# arm64: StampedPtr must not sign-extend bit 47 (user pointers have it set); crc32c uses the ARMv8 CRC instructions
# while crc32c_combine stays on its software combiner (folly's hardware combiner is x86-only).
if git -C ../third_party/folly apply --reverse --check ../../patches/folly-arm64.patch &>/dev/null; then
    echo "folly arm64 patch already applied. skipping."
else
    git -C ../third_party/folly apply ../../patches/folly-arm64.patch
fi
