target_add_lib(meta core-app core-user core-service fdb meta-fbs mgmtd-client storage-client memory-common analytics)
target_add_bin(meta_main "meta.cpp" meta jemalloc)
