# GDR (GPU Direct RDMA) Tests
#
# All CUDA/GDR-dependent test sources are consolidated here for unified build control.
# Hardware-dependent tests use GTEST_SKIP when runtime support is unavailable.
#
# target_add_test() picks up local *.cc files. The FUSE parser/metadata test
# stays in tests/fuse and is added explicitly below.

if(HF3FS_ENABLE_GDR)
  # libfuse3 search paths (mirrored from src/fuse/CMakeLists.txt)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    link_directories(/usr/local/lib/x86_64-linux-gnu/ /usr/lib64 /usr/local/lib64)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    link_directories(/usr/local/lib/aarch64-linux-gnu/ /usr/lib64 /usr/local/lib64)
  endif()

  target_add_test(test_gdr hf3fs_api common hf3fs_fuse fdb mgmtd-fbs)
  target_sources(test_gdr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fuse/TestIovTableGdr.cc)
  target_add_gdr_support(test_gdr SCOPE PRIVATE)
  target_include_directories(test_gdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # For tests/GtestHelpers.h
    ${CMAKE_SOURCE_DIR}/src         # For source headers
  )
endif()
