target_add_lib(mgmtd core-app core-user core-service fdb mgmtd-fbs memory-common)
target_add_bin(mgmtd_main "mgmtd.cpp" mgmtd)

