if (ENABLE_FUSE_APPLICATION)
add_custom_target(dump-config
            COMMENT "Running dump config"
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' -o -name 'admin_cli' -o -name 'hf3fs_fuse_main' | xargs -I {} bash -c '{} --dump_default_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`.toml'
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' -o -name 'hf3fs_fuse_main' | xargs -I {} bash -c '{} --dump_default_app_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`_app.toml'
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' -o -name 'hf3fs_fuse_main' | xargs -I {} bash -c '{} --dump_default_launcher_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`_launcher.toml')
add_dependencies(dump-config meta_main mgmtd_main storage_main admin_cli hf3fs_fuse_main)
else()
add_custom_target(dump-config
            COMMENT "Running dump config"
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' -o -name 'admin_cli' | xargs -I {} bash -c '{} --dump_default_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`.toml'
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' | xargs -I {} bash -c '{} --dump_default_app_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`_app.toml'
            COMMAND find ${CMAKE_CURRENT_BINARY_DIR} -name 'meta_main' -o -name 'mgmtd_main' -o -name 'storage_main' | xargs -I {} bash -c '{} --dump_default_launcher_cfg > ${CMAKE_SOURCE_DIR}/configs/`basename {}`_launcher.toml')
add_dependencies(dump-config meta_main mgmtd_main storage_main admin_cli)
endif()
