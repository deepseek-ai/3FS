target_add_lib(hf3fs_api client-lib-common storage-client numa rt)
target_add_shared_lib(hf3fs_api_shared client-lib-common storage-client numa rt)

# Add CUDA/GDR support if enabled
if(HF3FS_ENABLE_GDR)
    target_add_gdr_support(hf3fs_api SCOPE PUBLIC)
    target_add_gdr_support(hf3fs_api_shared SCOPE PUBLIC)
endif()

add_custom_command(
    TARGET hf3fs_api_shared
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_CURRENT_BINARY_DIR}/libhf3fs_api_shared.so" "${CMAKE_SOURCE_DIR}/src/lib/rs/hf3fs-usrbio-sys/lib/libhf3fs_api_shared.so"
    COMMENT "linking usrbio library to rust binding lib dir"
)
