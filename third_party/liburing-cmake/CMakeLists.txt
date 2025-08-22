set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../liburing/src")

add_library(3fs_liburing STATIC
  "${SRC_DIR}/ffi.c"
  "${SRC_DIR}/queue.c"
  "${SRC_DIR}/register.c"
  "${SRC_DIR}/setup.c"
  "${SRC_DIR}/syscall.c"
  "${SRC_DIR}/version.c"
)
target_include_directories(3fs_liburing SYSTEM PUBLIC "${SRC_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(3fs_liburing PRIVATE "${SRC_DIR}")
target_compile_definitions(3fs_liburing PRIVATE _GNU_SOURCE)
target_compile_definitions(3fs_liburing PRIVATE _LARGEFILE_SOURCE)
target_compile_definitions(3fs_liburing PRIVATE _FILE_OFFSET_BITS=64)
target_compile_definitions(3fs_liburing PRIVATE LIBURING_INTERNAL)
