include(${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake)
CheckGitSetup(${PROJECT_SOURCE_DIR})

set(FLATBUFFERS_FLATC_SCHEMA_EXTRA_ARGS --cpp-std=c++17)

target_add_lib(common memory-common version-info folly ibverbs scn::scn clickhouse-cpp-lib-static toml11 libzstd_static 3fs_liburing)
add_dependencies(common MonitorCollectorService-fbs)
target_sources(common PRIVATE utils/Linenoise.c)

# Add CUDA/GDR support if enabled
if(HF3FS_ENABLE_GDR)
    target_add_gdr_support(common SCOPE PRIVATE)
endif()

target_add_shared_lib(hf3fs_common_shared memory-common version-info folly ibverbs scn::scn clickhouse-cpp-lib-static toml11 libzstd_static 3fs_liburing)
add_dependencies(hf3fs_common_shared MonitorCollectorService-fbs)

# Add CUDA/GDR support to shared library if enabled
if(HF3FS_ENABLE_GDR)
    target_add_gdr_support(hf3fs_common_shared SCOPE PRIVATE)
endif()
