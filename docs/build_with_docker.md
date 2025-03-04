If you want to compile 3FS with docker, here is a guide for you.

# Step 1: create container

> run command in host

```bash
$ mkdir build-3fs; cd build-3fs
$ wget https://raw.githubusercontent.com/deepseek-ai/3FS/refs/heads/main/scripts/playground.sh
$ sudo bash playground.sh
```

# Step2: install dependencies

> run command in container

## 2.1 install dependencies
```bash
$ apt update
$ apt install -y cmake libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev \
    libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev clang-format-14 clang-14 clang-tidy-14 lld-14 \
    libgoogle-perftools-dev google-perftools libssl-dev gcc-12 g++-12 libboost-all-dev cargo git g++ wget meson
```

## 2.2 install foundationdb

```shell
$ cd ${BUILD_DIR}
$ wget https://github.com/apple/foundationdb/releases/download/7.1.67/foundationdb-server_7.1.67-1_amd64.deb \
    https://github.com/apple/foundationdb/releases/download/7.1.67/foundationdb-clients_7.1.67-1_amd64.deb
$ dpkg -i foundationdb-server_7.1.67-1_amd64.deb foundationdb-clients_7.1.67-1_amd64.deb
```

## 2.3 build and install fuse

```bash
$ cd ${BUILD_DIR}
$ wget https://github.com/libfuse/libfuse/releases/download/fuse-3.16.2/fuse-3.16.2.tar.gz
$ tar -zxvf fuse-3.16.2.tar.gz
$ cd fuse-3.16.2; mkdir build; cd build
$ meson setup ..
$ ninja
$ ninja install  # ignore error
```

# Step 3: build 3FS

> run command in container

```bash
$ cd ${BUILD_DIR}
$ git clone https://github.com/deepseek-ai/3fs
$ cd 3fs
$ git submodule update --init --recursive
$ ./patches/apply.sh
$ cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_C_COMPILER=clang-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
$ cmake --build build -j 45
```

It will generates binrary shown as below once build success:
```shell
$ cd ${BUILD_DIR}; ls -ls 3fs/build/bin
total 2308428
355344 -rwxr-xr-x 1 root root 363871904 Mar  4 11:36 admin_cli
144976 -rwxr-xr-x 1 root root 148454880 Mar  4 11:30 hf3fs-admin
204336 -rwxr-xr-x 1 root root 209239320 Mar  4 11:32 hf3fs_fuse_main
277812 -rwxr-xr-x 1 root root 284476352 Mar  4 11:30 meta_main
174700 -rwxr-xr-x 1 root root 178892200 Mar  4 11:27 mgmtd_main
168300 -rwxr-xr-x 1 root root 172336688 Mar  4 11:26 migration_main
102740 -rwxr-xr-x 1 root root 105205000 Mar  4 11:19 monitor_collector_main
170628 -rwxr-xr-x 1 root root 174721688 Mar  4 11:26 simple_example_main
395964 -rwxr-xr-x 1 root root 405484072 Mar  4 11:34 storage_bench
313628 -rwxr-xr-x 1 root root 321173936 Mar  4 11:28 storage_main
```