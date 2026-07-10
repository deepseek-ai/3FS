target_add_test(test_common common client-lib-common fdb mgmtd-fbs)

if(NOT HF3FS_GDR_AVAILABLE)
  target_sources(test_common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../gdr/TestGdrOffCompile.cc)
  target_link_libraries(test_common hf3fs_api)
endif()
