FROM ubuntu2004-3fs-builder:latest as builder

WORKDIR /3fs
COPY . .
#RUN cmake -S . -B build_dir -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_C_COMPILER=clang-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \ 
#    && cmake --build build_dir -j `nproc`

FROM ubuntu:20.04
RUN sed -i 's/archive.ubuntu.com/mirrors.tuna.tsinghua.edu.cn/g' /etc/apt/sources.list
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends wget ca-certificates systemd vim net-tools gdb\
    libboost1.71-all-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libdwarf-dev \
    libdouble-conversion-dev \
    libaio-dev  \ 
    libgoogle-perftools-dev  \ 
    && apt-get clean   \
    && rm -rf /var/lib/apt/lists/*


ARG FDB_VERSION=7.3.63
RUN FDB_ARCH_SUFFIX=$(dpkg --print-architecture) && \
    case "${FDB_ARCH_SUFFIX}" in \
      amd64) ;; \
      arm64) FDB_ARCH_SUFFIX="aarch64" ;; \ 
      *) echo "Unsupported architecture: ${FDB_ARCH_SUFFIX}"; exit 1 ;; \
      esac && \
      FDB_CLIENT_URL="https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb" && \
      FDB_SERVER_URL="https://github.com/apple/foundationdb/releases/download/${FDB_VERSION}/foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb" && \
      wget -q "${FDB_CLIENT_URL}" && \
      wget -q "${FDB_SERVER_URL}" && \
      dpkg -i foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb 
      # dpkg -i --force-all foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb 
      # rm foundationdb-clients_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb 
      # rm foundationdb-server_${FDB_VERSION}-1_${FDB_ARCH_SUFFIX}.deb

RUN wget https://raw.githubusercontent.com/Mellanox/container_scripts/refs/heads/master/ibdev2netdev -O /usr/sbin/ibdev2netdev && \
chmod +x /usr/sbin/ibdev2netdev

RUN mkdir -p /opt/3fs/bin && mkdir -p /opt/3fs/etc && mkdir -p /opt/3fs/scripts && mkdir -p /var/log/3fs
COPY --from=builder /3fs/build_dir/bin/storage_main   /opt/3fs/bin/
COPY --from=builder /3fs/configs/storage_main*.toml /opt/3fs/etc/
COPY --from=builder /3fs/deploy/systemd/storage_main.service /usr/lib/systemd/system/
COPY --from=builder /3fs/build_dir/third_party/jemalloc/lib/libjemalloc.so.2 /usr/lib/

COPY --from=builder /3fs/deploy/scripts/_3fs_common.sh /opt/3fs/scripts/
COPY --from=builder /3fs/deploy/scripts/start_storage.sh /opt/3fs/scripts/

COPY --from=builder /3fs/build_dir/bin/admin_cli /opt/3fs/bin/
COPY --from=builder /3fs/configs/admin_cli.toml /opt/3fs/etc/

WORKDIR /opt/3fs/bin

CMD ["/opt/3fs/scripts/start_storage.sh"]

