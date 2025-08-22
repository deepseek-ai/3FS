add_library(leveldb_logger STATIC LevelDBLogger.cpp)
target_link_libraries(leveldb_logger common leveldb)
target_compile_options(leveldb_logger PRIVATE "-fno-rtti")

target_add_lib(kv common leveldb leveldb_logger rocksdb)
target_include_directories(kv PUBLIC ${CMAKE_SOURCE_DIR}/third_party/rocksdb/include)
