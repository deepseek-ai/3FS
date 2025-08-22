FROM opencloudos/opencloudos9-minimal:latest
RUN dnf clean all \
    && dnf install -y epol-release \
    && dnf install -y wget git meson cmake perl lld gcc gcc-c++ autoconf lz4 lz4-devel xz xz-devel \
       double-conversion-devel libdwarf-devel libunwind-devel libaio-devel gflags-devel glog-devel \
       libuv-devel gmock-devel gperftools gperftools-devel openssl-devel boost-static boost-devel mono-devel \
       libevent-devel libibverbs-devel numactl-devel python3-devel \
    && dnf remove -y fuse \
    && dnf clean all \
    && echo "alias ll='ls -al'" >> /root/.bashrc

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain 1.85.0
ENV PATH="/root/.cargo/bin:${PATH}"

RUN mkdir -p /tmp/fuse \
    && cd /tmp/fuse \
    && wget https://github.com/libfuse/libfuse/releases/download/fuse-3.16.2/fuse-3.16.2.tar.gz \
    && tar -zxf fuse-3.16.2.tar.gz \
    && cd fuse-3.16.2 \
    && mkdir build \
    && cd build \
    && meson setup .. \
    && meson configure -D default_library=both \
    && meson setup --reconfigure ../ \
    && ninja \
    && ninja install \
    && cd / \
    && rm -rf /tmp/fuse
RUN mkdir -p /tmp/foundationdb \
    && cd /tmp/foundationdb \
    && wget https://github.com/apple/foundationdb/releases/download/7.3.63/foundationdb-clients-7.3.63-1.el7.x86_64.rpm \
    && wget https://github.com/apple/foundationdb/releases/download/7.3.63/foundationdb-server-7.3.63-1.el7.x86_64.rpm \
    && rpm -ivh foundationdb-clients-7.3.63-1.el7.x86_64.rpm \
    && rpm -ivh foundationdb-server-7.3.63-1.el7.x86_64.rpm \
    && cd / \
    && rm -rf /tmp/foundationdb
RUN mkdir -p /tmp/clang \
    && cd /tmp/clang \
    && wget https://github.com/llvm/llvm-project/releases/download/llvmorg-14.0.6/clang+llvm-14.0.6-x86_64-linux-gnu-rhel-8.4.tar.xz \
    && tar -xf clang+llvm-14.0.6-x86_64-linux-gnu-rhel-8.4.tar.xz \
    && mv clang+llvm-14.0.6-x86_64-linux-gnu-rhel-8.4 /usr/local/clang-llvm-14 \
    && ln -s /usr/local/clang-llvm-14/bin/clang++ /usr/local/clang-llvm-14/bin/clang++-14 \
    && ln -s /usr/local/clang-llvm-14/bin/clang-format /usr/local/clang-llvm-14/bin/clang-format-14 \
    && ln -s /usr/local/clang-llvm-14/bin/clang-tidy /usr/local/clang-llvm-14/bin/clang-tidy-14 \
    && ln -s /usr/local/clang-llvm-14/bin/clang-format /usr/bin/clang-format-14 \
    && ln -s /lib/gcc/x86_64-TencentOS-linux /lib/gcc/x86_64-linux-gnu \
    && ln -s /usr/include/c++/12/x86_64-TencentOS-linux /usr/include/c++/12/x86_64-linux-gnu \
    && ln -s /usr/libexec/gcc/x86_64-TencentOS-linux /usr/libexec/gcc/x86_64-linux-gnu \
    && echo "export PATH=\$PATH:/usr/local/clang-llvm-14/bin" >> /root/.bashrc \
    && cd / \
    && rm -rf /tmp/clang
CMD ["/bin/bash"]