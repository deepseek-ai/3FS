add_library(mgmtd-test-helper STATIC MgmtdTestHelper.cpp)
target_link_libraries(mgmtd-test-helper mgmtd)
target_add_test(test_mgmtd mgmtd-test-helper fdb)
