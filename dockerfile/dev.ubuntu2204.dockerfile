FROM ubuntu:22.04

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
RUN apt-get update                                    &&\
  apt-get install -y --no-install-recommends            \
  git wget ca-certificates                              \
  clang-format-14 clang-14 clang-tidy-14 lld-14         \
  build-essential meson gcc-12 g++-12 cmake rustc cargo \
  google-perftools                                      \
  libaio-dev                                            \
  libboost-all-dev                                      \
  libdouble-conversion-dev                              \
  libdwarf-dev                                          \
  libgflags-dev                                         \
  libgmock-dev                                          \
  libgoogle-glog-dev                                    \
  libgoogle-perftools-dev                               \
  libgtest-dev                                          \
  liblz4-dev                                            \
  liblzma-dev                                           \
  libssl-dev                                            \
  libunwind-dev                                         \
  libuv1-dev                                          &&\
  apt-get clean                                       &&\
  rm -rf /var/lib/apt/lists/*

  ARG TARGETARCH
  ARG FDB_VERSION=7.3.63
  ARG FDB_ARCH_SUFFIX
  RUN case "${TARGETARCH}" in \
      amd64) FDB_ARCH_SUFFIX="amd64" ;; \
      arm64) FDB_ARCH_SUFFIX="aarch64" ;; \
      *) echo "Unsupported architecture: ${TARGETARCH}"; exit 1 ;; \
      esac && \
      FDB_CLIENT_URL="https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb" && \
      FDB_SERVER_URL="https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb" && \
      wget -q "${FDB_CLIENT_URL}" && \
      wget -q "${FDB_SERVER_URL}" && \
      dpkg -i foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb && \
      dpkg -i foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb && \
      rm foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb

ARG LIBFUSE_VERSION=3.16.2
ARG LIBFUSE_DOWNLOAD_URL=https://github.com/libfuse/libfuse/releases/download/fuse-${LIBFUSE_VERSION}/fuse-${LIBFUSE_VERSION}.tar.gz
RUN wget -O- ${LIBFUSE_DOWNLOAD_URL}        |\
  tar -xzvf - -C /tmp                      &&\
  cd /tmp/fuse-${LIBFUSE_VERSION}          &&\
  mkdir build && cd build                  &&\
  meson setup .. && meson configure -D default_library=both &&\
  ninja && ninja install &&\
  rm -f -r /tmp/fuse-${LIBFUSE_VERSION}*