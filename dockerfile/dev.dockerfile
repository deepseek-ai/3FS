FROM ubuntu:22.04

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
RUN apt-get update && apt-get install -y locales \
  && sed -i 's/# zh_CN.UTF-8 UTF-8/zh_CN.UTF-8 UTF-8/' /etc/locale.gen \
  && locale-gen \
  && DEBIAN_FRONTEND=noninteractive apt install -y tzdata \
  && ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime \
  && dpkg-reconfigure --frontend noninteractive tzdata \
  && apt-get install -y                                 \
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
RUN echo "当前的架构是 ${TARGETARCH}" &&\
  if [ "${TARGETARCH}" = "amd64" ]; then\
    echo "当前的架构是 amd64";\
    wget https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_amd64.deb &&\
    wget https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-server_${FDB_VERSION}-1_amd64.deb &&\
    dpkg -i foundationdb-clients_${FDB_VERSION}-1_amd64.deb &&\
    dpkg -i foundationdb-server_${FDB_VERSION}-1_amd64.deb &&\
    rm foundationdb-clients_${FDB_VERSION}-1_amd64.deb &&\
    rm foundationdb-server_${FDB_VERSION}-1_amd64.deb;\
  elif [ "${TARGETARCH}" = "arm64" ]; then\
    echo "当前的架构是 arm64";\
    wget https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_aarch64.deb &&\
    wget https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-server_${FDB_VERSION}-1_aarch64.deb &&\
    dpkg -i foundationdb-clients_${FDB_VERSION}-1_aarch64.deb &&\
    dpkg -i foundationdb-server_${FDB_VERSION}-1_aarch64.deb &&\
    rm foundationdb-clients_${FDB_VERSION}-1_aarch64.deb &&\
    rm foundationdb-server_${FDB_VERSION}-1_aarch64.deb;\
  else\
    echo "未知的架构" && exit 1;\
  fi
ARG LIBFUSE_VERSION=3.16.2
ARG LIBFUSE_DOWNLOAD_URL=https://github.com/libfuse/libfuse/releases/download/fuse-${LIBFUSE_VERSION}/fuse-${LIBFUSE_VERSION}.tar.gz
RUN wget -O- ${LIBFUSE_DOWNLOAD_URL}        |\
  tar -xzvf - -C /tmp                      &&\
  cd /tmp/fuse-${LIBFUSE_VERSION}          &&\
  mkdir build && cd build                  &&\
  meson setup .. && meson configure -D default_library=both &&\
  ninja && ninja install &&\
  rm -f -r /tmp/fuse-${LIBFUSE_VERSION}*

ENV LANG=zh_CN.UTF-8
ENV LANGUAGE=zh_CN:zh
ENV LC_ALL=zh_CN.UTF-8