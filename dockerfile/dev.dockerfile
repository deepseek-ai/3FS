FROM ubuntu:22.04

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
RUN apt-get update                                    &&\
  apt-get install -y --no-install-recommends            \
  wget ca-certificates                                  \
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
ARG FDB_VERSION=7.1.61
ARG FDB_DOWNLOAD_URL=https://github.com/apple/foundationdb/releases/download/7.1.61/foundationdb-clients_${FDB_VERSION}-1_amd64.deb
RUN wget ${FDB_DOWNLOAD_URL} -O /tmp/foundationdb-clients.deb &&\
  dpkg -i /tmp/foundationdb-clients.deb                       &&\
  rm /tmp/foundationdb-clients.deb
ARG LIBFUSE_VERSION=3.16.2
ARG LIBFUSE_DOWNLOAD_URL=https://github.com/libfuse/libfuse/releases/download/fuse-${LIBFUSE_VERSION}/fuse-${LIBFUSE_VERSION}.tar.gz
RUN wget -O- ${LIBFUSE_DOWNLOAD_URL}        |\
  tar -xzvf - -C /tmp                      &&\
  cd /tmp/fuse-${LIBFUSE_VERSION}          &&\
  mkdir build && cd build                  &&\
  meson setup .. && ninja && ninja install &&\
  rm -f -r /tmp/fuse-${LIBFUSE_VERSION}*
