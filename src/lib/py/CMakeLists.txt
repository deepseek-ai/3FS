pybind11_add_module(hf3fs_py_usrbio usrbio_binding.cc)

target_compile_definitions(hf3fs_py_usrbio
                           PRIVATE) # VERSION_INFO=${PYCLIENT_VERSION_INFO})

target_include_directories(hf3fs_py_usrbio
  PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_BINARY_DIR}/src
  ${PROJECT_BINARY_DIR}
  )
target_link_libraries(hf3fs_py_usrbio PRIVATE hf3fs_api_shared)

