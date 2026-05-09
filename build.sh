#!/bin/bash
# 3FS Unified Build Script

set -e

# Configuration
BUILD_TYPE=${BUILD_TYPE:-Release}
INSTALL_PREFIX=${INSTALL_PREFIX:-/usr/local}
ENABLE_TESTS=${ENABLE_TESTS:-OFF}
ENABLE_DOCS=${ENABLE_DOCS:-OFF}

echo "=== 3FS Build Script ==="
echo "Build type: $BUILD_TYPE"
echo "Install prefix: $INSTALL_PREFIX"

# Clean build directory
if [ "$1" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf build
    exit 0
fi

# Create build directory
mkdir -p build
cd build

# Run CMake
cmake .. \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX \
    -DENABLE_TESTS=$ENABLE_TESTS \
    -DENABLE_DOCS=$ENABLE_DOCS

# Compile
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Run tests if enabled
if [ "$ENABLE_TESTS" = "ON" ]; then
    ctest --output-on-failure
fi

# Install if requested
if [ "$1" = "install" ]; then
    make install
    echo "Installed to: $INSTALL_PREFIX"
fi

echo "Build completed!"
