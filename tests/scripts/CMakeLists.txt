add_custom_target(fdb_loopback ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fdb_loopback.sh)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fdb_loopback.sh
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/fdb_loopback.sh
                                     ${CMAKE_CURRENT_BINARY_DIR}/fdb_loopback.sh
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fdb_loopback.sh)
