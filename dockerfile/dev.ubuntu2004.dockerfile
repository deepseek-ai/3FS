FROM ubuntu:20.04

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update                                    &&\
  apt-get install -y --no-install-recommends            \
  git wget ca-certificates software-properties-common   \
  build-essential meson cmake                           \
  google-perftools                                      \
  libaio-dev                                            \
  libboost1.71-all-dev                                  \
  libdouble-conversion-dev                              \
  libdwarf-dev                                          \
  libgflags-dev                                         \
  libgmock-dev                                          \
  libgoogle-glog-dev                                    \
  libgoogle-perftools-dev                               \
  libgtest-dev                                          \
  liblz4-dev                                            \
  liblzma-dev                                           \
  libunwind-dev                                         \
  libuv1-dev                                            \
  libssl-dev                                            \
  gnupg                                                &&\
  apt-get clean                                       &&\
  rm -rf /var/lib/apt/lists/*

RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key |tee /etc/apt/trusted.gpg.d/llvm.asc &&\
    add-apt-repository -y "deb http://apt.llvm.org/focal/ llvm-toolchain-focal-14 main" &&\
    apt-get update && apt-get install -y clang-format-14 clang-14 clang-tidy-14 lld-14 libclang-rt-14-dev gcc-10 g++-10 &&\
    apt-get clean &&\
    rm -rf /var/lib/apt/lists/*

ARG FDB_VERSION=7.3.63
RUN FDB_ARCH_SUFFIX=$(dpkg --print-architecture) && \
    case "${FDB_ARCH_SUFFIX}" in \
      amd64) ;; \
      arm64) FDB_ARCH_SUFFIX="aarch64" ;; \ 
      *) echo "Unsupported architecture: ${FDB_ARCH_SUFFIX}"; exit 1 ;; \
      esac && \
      FDB_CLIENT_URL="https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb" && \
      wget -q "${FDB_CLIENT_URL}" && \
      dpkg -i foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb && \
      rm foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb 

ARG LIBFUSE_VERSION=3.16.2
ARG LIBFUSE_DOWNLOAD_URL=https://github.com/libfuse/libfuse/releases/download/fuse-${LIBFUSE_VERSION}/fuse-${LIBFUSE_VERSION}.tar.gz
RUN wget -O- ${LIBFUSE_DOWNLOAD_URL}        |\
  tar -xzvf - -C /tmp                      &&\
  cd /tmp/fuse-${LIBFUSE_VERSION}          &&\
  mkdir build && cd build                  &&\
  meson setup .. && meson configure -D default_library=both &&\
  ninja && ninja install &&\
  rm -f -r /tmp/fuse-${LIBFUSE_VERSION}*

# Install Rust
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"


