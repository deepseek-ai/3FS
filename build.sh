#!/bin/bash
set -e

# Default build config
OS_TYPE="ubuntu2204"

# Parse arguments
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  cat <<EOF
HF3FS Build System

Usage: $0 [OPTION]

Options:
  docker-ubuntu2204    Build using Ubuntu 22.04 Docker container (default)
  docker-ubuntu2004    Build using Ubuntu 20.04 Docker container  
  docker-centos9       Build using CentOS 9 Docker container
  -h, --help           Show this help message

Environment:
  - Docker builds create isolated environments with version-specific toolchains
  - Build artifacts are stored in: build/

Examples:
  ./build.sh                    # Default Docker build with Ubuntu 22.04
  ./build.sh docker-ubuntu2004  # Docker build with Ubuntu 20.04

EOF
  exit 0

elif [[ "$1" == "docker-ubuntu2204" ]]; then
  OS_TYPE="ubuntu2204"
elif [[ "$1" == "docker-ubuntu2004" ]]; then
  OS_TYPE="ubuntu2004"
elif [[ "$1" == "docker-centos9" ]]; then
  OS_TYPE="centos9" 
elif [[ -n "$1" ]]; then
  echo "Error: Invalid option '$1'"
  echo "Try './build.sh --help' for usage information"
  exit 1
fi

# Common build parameters
CPU_CORES=$(nproc)
CMAKE_FLAGS=(
  -DCMAKE_CXX_COMPILER=clang++-14
  -DCMAKE_C_COMPILER=clang-14
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

docker_build() {
  echo "Starting Docker build for ${OS_TYPE}..."
  DOCKER_IMAGE="${OS_TYPE}-3fs-builder"
  docker build -t "${DOCKER_IMAGE}" -f "dockerfile/dev.${OS_TYPE}.dockerfile" . && \
  docker run --rm \
    -v "${PWD}:/build/src" \
    --cpus="${CPU_CORES}" \
    -e BUILD_JOBS="${CPU_CORES}" \
    "${DOCKER_IMAGE}" /bin/bash -c "
      set -ex
      cd /build/src
      cmake -S . -B build ${CMAKE_FLAGS[*]}
      cmake --build build -j\${BUILD_JOBS}
    "
}


# Execute build
docker_build || echo "Docker build failed"
