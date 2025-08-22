target_add_lib(migration core-app core-user core-service fdb migration-fbs mgmtd-client storage-client memory-common analytics)
target_add_bin(migration_main "main.cpp" migration)
