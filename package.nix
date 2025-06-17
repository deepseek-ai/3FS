{ stdenv
, lib
, fetchFromGitHub
, runCommand
, writeText
, makeWrapper
, cmake
, pkg-config
, clang_14
, lld_14
, rustc
, cargo
, rustPlatform
, autoconf
, automake
, libtool
, python3
, boost
, libuv
, lz4
, xz
, double-conversion
, libdwarf
, libunwind
, libaio
, gflags
, glog
, gtest
, gperftools
, openssl
, fuse3
, foundationdb
, rdma-core
, libibverbs
, zstd
, jemalloc
, libevent
, numactl
, liburing
, thrift
, bison
, flex
, git
, which
, fmt
, folly
, rocksdb
, leveldb
, arrow-cpp
, mimalloc
, clickhouse-cpp
, tomlplusplus
, scnlib
}:

let
  # Build jemalloc with specific configuration for 3FS
  jemalloc-custom = jemalloc.overrideAttrs (oldAttrs: {
    configureFlags = (oldAttrs.configureFlags or []) ++ [
      "--disable-cxx"
      "--enable-prof"
      "--disable-initial-exec-tls"
    ];
  });
  
  # Custom FoundationDB build if needed
  fdb-version = "7.1.5";
  
in stdenv.mkDerivation rec {
  pname = "3fs";
  version = "0.1.5";

  src = fetchFromGitHub {
    owner = "deepseek-ai";
    repo = "3fs";
    rev = "91bfcf39a9e4b5ded959f7b5c2cb0cf858ebbff5";
    sha256 = "sha256-0000000000000000000000000000000000000000000=";  # To be replaced
    fetchSubmodules = true;
  };

  nativeBuildInputs = [
    cmake
    pkg-config
    clang_14
    lld_14
    rustc
    cargo
    rustPlatform.cargoSetupHook
    autoconf
    automake
    libtool
    python3
    git
    which
    bison
    flex
  ];

  buildInputs = [
    # Core dependencies
    boost
    libuv
    lz4
    xz
    double-conversion
    libdwarf
    libunwind
    libaio
    gflags
    glog
    gtest
    gperftools
    openssl
    
    # FUSE 3
    fuse3
    
    # FoundationDB
    foundationdb
    
    # RDMA/InfiniBand
    rdma-core
    libibverbs
    
    # Additional libraries
    zstd
    jemalloc-custom
    libevent
    numactl
    liburing
    thrift
    fmt
    folly
    rocksdb
    leveldb
    mimalloc
    tomlplusplus
    
    # Python bindings
    python3.pkgs.pybind11
  ];

  patches = [
    # Add any necessary patches here
  ];

  postPatch = ''
    # Apply the repository patches
    patchShebangs ./patches/apply.sh
    ./patches/apply.sh || true
    
    # Fix CMake to use system libraries
    substituteInPlace CMakeLists.txt \
      --replace 'add_link_options(-fuse-ld=lld)' '# add_link_options(-fuse-ld=lld)' \
      --replace 'set(CMAKE_CXX_FLAGS "$' 'set(CMAKE_CXX_FLAGS "$'
    
    # Disable bundled third-party libraries
    for lib in fmt zstd googletest folly leveldb rocksdb scnlib pybind11 toml11 mimalloc clickhouse-cpp liburing-cmake; do
      substituteInPlace CMakeLists.txt \
        --replace "add_subdirectory(\"third_party/$lib\"" "#add_subdirectory(\"third_party/$lib\""
    done
    
    # Fix jemalloc build
    substituteInPlace cmake/Jemalloc.cmake \
      --replace 'ExternalProject_add(' 'return() #ExternalProject_add(' \
      --replace "\''${JEMALLOC_DIR}/lib/libjemalloc.so.2" "\''${jemalloc-custom}/lib/libjemalloc.so"
    
    # Fix Apache Arrow build
    substituteInPlace cmake/ApacheArrow.cmake \
      --replace 'ExternalProject_Add(' 'return() #ExternalProject_Add('
    
    # Set up Rust dependencies
    cd src/client/trash_cleaner
    cargoDepsCopy="$NIX_BUILD_TOP/cargo-vendor-dir"
    if [ -d "$cargoDepsCopy" ]; then
      chmod -R +w "$cargoDepsCopy"
    fi
    cd $NIX_BUILD_TOP/source
    
    # Fix storage chunk_engine Rust build
    cd src/storage/chunk_engine
    cargoDepsCopy="$NIX_BUILD_TOP/cargo-vendor-dir"
    if [ -d "$cargoDepsCopy" ]; then
      chmod -R +w "$cargoDepsCopy"
    fi
    cd $NIX_BUILD_TOP/source
  '';

  preConfigure = ''
    # Create necessary directories
    mkdir -p $TMP/third_party
    
    # Setup environment
    export HOME=$TMP
    export CARGO_HOME=$TMP/.cargo
    
    # Set up library paths
    export BOOST_ROOT=${boost}
    export BOOST_INCLUDEDIR=${boost}/include
    export BOOST_LIBRARYDIR=${boost}/lib
    
    # FoundationDB paths
    export FDB_LIBRARY_DIR=${foundationdb}/lib
    export FDB_INCLUDE_DIR=${foundationdb}/include
    
    # Jemalloc paths
    export JEMALLOC_OVERRIDE=${jemalloc-custom}/lib/libjemalloc.so
    export JEMALLOC_DIR=${jemalloc-custom}
    
    # Compiler flags
    export CXXFLAGS="-I${boost}/include -I${gtest}/include -I${liburing}/include -I${fmt}/include -I${folly}/include"
    export LDFLAGS="-L${boost}/lib -L${jemalloc-custom}/lib -L${liburing}/lib -L${fmt}/lib -L${folly}/lib"
  '';

  cmakeFlags = [
    "-DCMAKE_CXX_COMPILER=${clang_14}/bin/clang++"
    "-DCMAKE_C_COMPILER=${clang_14}/bin/clang"
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DENABLE_FUSE_APPLICATION=ON"
    "-DOVERRIDE_CXX_NEW_DELETE=OFF"
    "-DSAVE_ALLOCATE_SIZE=OFF"
    "-DBoost_USE_STATIC_LIBS=ON"
    "-DFDB_VERSION=${fdb-version}"
    
    # System library paths
    "-DCMAKE_PREFIX_PATH=${boost};${gtest};${liburing};${fmt};${folly}"
    "-DBoost_DIR=${boost}/lib/cmake/Boost"
    "-DBoost_INCLUDE_DIR=${boost}/include"
    "-Dfmt_DIR=${fmt}/lib/cmake/fmt"
    "-DFolly_DIR=${folly}/lib/cmake/folly"
    "-DGTest_DIR=${gtest}/lib/cmake/GTest"
    "-DZSTD_LIBRARY=${zstd}/lib/libzstd.so"
    "-DZSTD_INCLUDE_DIR=${zstd}/include"
    "-Djemalloc_INCLUDE_DIR=${jemalloc-custom}/include"
    "-Djemalloc_LIBRARY=${jemalloc-custom}/lib/libjemalloc.so"
    "-DJEMALLOC_DIR=${jemalloc-custom}"
    "-DARROW_INCLUDE_DIR=${arrow-cpp}/include"
    "-DARROW_LIB_DIR=${arrow-cpp}/lib"
    "-Darrow_DIR=${arrow-cpp}/lib/cmake/arrow"
    
    # Initially disable tests to simplify build
    "-DBUILD_TESTING=OFF"
  ];

  # Custom build phase to handle complex build process
  buildPhase = ''
    runHook preBuild
    
    # Build with decreasing parallelism if needed
    cmake --build . -j $NIX_BUILD_CORES || \
    cmake --build . -j 4 || \
    cmake --build . -j 1
    
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    
    # Create output directories
    mkdir -p $out/bin $out/lib $out/etc/3fs
    
    # Install binaries
    for bin in \
      src/fuse/hf3fs_fuse \
      src/client/cli/admin/admin \
      src/meta/meta \
      src/mgmtd/mgmtd \
      src/storage/storage \
      src/monitor_collector/monitor_collector \
      src/tools/admin
    do
      if [ -f "$bin" ] && [ -x "$bin" ]; then
        install -D -m755 "$bin" "$out/bin/$(basename $bin)"
      fi
    done
    
    # Rename main binaries to match systemd service expectations
    [ -f "$out/bin/meta" ] && mv "$out/bin/meta" "$out/bin/meta_main"
    [ -f "$out/bin/mgmtd" ] && mv "$out/bin/mgmtd" "$out/bin/mgmtd_main"
    [ -f "$out/bin/storage" ] && mv "$out/bin/storage" "$out/bin/storage_main"
    [ -f "$out/bin/monitor_collector" ] && mv "$out/bin/monitor_collector" "$out/bin/monitor_collector_main"
    [ -f "$out/bin/hf3fs_fuse" ] && cp "$out/bin/hf3fs_fuse" "$out/bin/hf3fs_fuse_main"
    
    # Install libraries
    find . -name "*.so" -o -name "*.so.*" | while read lib; do
      if [ -f "$lib" ]; then
        install -D -m644 "$lib" "$out/lib/$(basename $lib)"
      fi
    done
    
    # Install static libraries (optional)
    find . -name "*.a" | while read lib; do
      if [ -f "$lib" ] && [[ ! "$lib" =~ third_party ]]; then
        install -D -m644 "$lib" "$out/lib/$(basename $lib)"
      fi
    done
    
    # Install Python modules
    if [ -d "src/lib/py" ]; then
      mkdir -p $out/lib/python${python3.pythonVersion}/site-packages
      cp -r src/lib/py/* $out/lib/python${python3.pythonVersion}/site-packages/
    fi
    
    # Install configuration files
    cp -r configs/* $out/etc/3fs/
    
    # Install headers (for development)
    mkdir -p $out/include/3fs
    find src -name "*.h" -o -name "*.hpp" | while read header; do
      rel_path=$(echo "$header" | sed 's|^src/||')
      install -D -m644 "$header" "$out/include/3fs/$rel_path"
    done
    
    runHook postInstall
  '';

  postFixup = ''
    # Fix RPATH for all binaries
    for exe in $out/bin/*; do
      if [ -f "$exe" ] && [ -x "$exe" ]; then
        patchelf --set-rpath "$out/lib:${lib.makeLibraryPath buildInputs}" "$exe" || true
      fi
    done
    
    # Fix RPATH for all libraries
    for lib in $out/lib/*.so*; do
      if [ -f "$lib" ]; then
        patchelf --set-rpath "$out/lib:${lib.makeLibraryPath buildInputs}" "$lib" || true
      fi
    done
    
    # Create wrapper scripts if needed
    for bin in $out/bin/*_main; do
      if [ -f "$bin" ]; then
        makeWrapper "$bin" "$bin.wrapped" \
          --prefix LD_LIBRARY_PATH : "$out/lib:${lib.makeLibraryPath buildInputs}" \
          --set RUST_BACKTRACE 1
        mv "$bin.wrapped" "$bin"
      fi
    done
  '';

  # Enable parallel building
  enableParallelBuilding = true;
  NIX_BUILD_CORES = 8;

  # Set up proper library paths
  setupHook = writeText "setup-hook" ''
    export LD_LIBRARY_PATH="$1/lib''${LD_LIBRARY_PATH:+:}$LD_LIBRARY_PATH"
  '';

  passthru = {
    inherit jemalloc-custom;
    tests = {
      # Add package tests here
      version = runCommand "3fs-version-test" {} ''
        ${pname}/bin/admin --version
        touch $out
      '';
    };
  };

  meta = with lib; {
    description = "Fire-Flyer File System - High-performance distributed file system for AI workloads";
    longDescription = ''
      3FS is a high-performance distributed file system designed to address the challenges
      of AI training and inference workloads. It leverages modern SSDs and RDMA networks
      to provide a shared storage layer that simplifies development of distributed applications.
    '';
    homepage = "https://github.com/deepseek-ai/3fs";
    license = licenses.mit;
    maintainers = with maintainers; [ ];
    platforms = platforms.linux;
    mainProgram = "admin";
  };
}