# Check if IPO is supported
include(CheckIPOSupported)
check_ipo_supported(RESULT HAVE_IPO)

# Enable IPO in non-debug build
macro(target_enable_ipo NAME)
    if(NOT CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG" AND HAVE_IPO)
        set_property(TARGET ${NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message (STATUS "Enabled IPO for target: ${NAME}")
    endif()
endmacro()

macro(target_add_lib NAME)
    file(GLOB_RECURSE FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.cc" "*.h")
    add_library(${NAME} STATIC ${FILES} ${FBS_FILES})
    target_include_directories(${NAME}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            ${PROJECT_SOURCE_DIR}
            ${PROJECT_BINARY_DIR}/src
            ${PROJECT_BINARY_DIR}
    )
    target_link_libraries(${NAME} ${ARGN} "")
endmacro()

macro(target_add_shared_lib NAME)
    file(GLOB_RECURSE FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.cc" "*.h")
    add_library(${NAME} SHARED ${FILES} ${FBS_FILES})
    target_include_directories(${NAME}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            ${PROJECT_SOURCE_DIR}
            ${PROJECT_BINARY_DIR}/src
            ${PROJECT_BINARY_DIR}
    )
    target_link_libraries(${NAME} ${ARGN} "")
    target_enable_ipo(${NAME})
endmacro()

macro(target_add_bin NAME MAIN_FILE)
  add_executable(${NAME} ${MAIN_FILE})
    target_link_libraries(${NAME} ${ARGN} "")
    target_include_directories(${NAME}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            ${PROJECT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/src/lib/api
            ${PROJECT_BINARY_DIR}/src
            ${PROJECT_BINARY_DIR}
    )
    set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_enable_ipo(${NAME})
endmacro()

macro(target_add_test NAME)
    file(GLOB_RECURSE FILES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.cc")
    add_executable(${NAME} ${FILES})
    target_link_libraries(${NAME} gmock test_main ${ARGN} "")
    target_include_directories(${NAME}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            ${PROJECT_SOURCE_DIR}
            ${PROJECT_BINARY_DIR}/src
            ${PROJECT_BINARY_DIR}
    )
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_target_properties(${NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endmacro()

macro(target_add_fbs NAME PATH)
    cmake_parse_arguments(FBS "SERVICE" "" "DEPS" ${ARGN})

    message("Fbs " ${NAME} " FBS_SERVICE " ${FBS_SERVICE} " ARGN " ${ARGN})
  
    set(FLATBUFFERS_FLATC_SCHEMA_EXTRA_ARGS
        --scoped-enums
        --gen-object-api
        --gen-mutable
        --gen-compare
        --cpp-std=c++17
        --python
        --hf3fs
        --keep-prefix
    )

    get_filename_component(NAME_WE ${PATH} NAME_WE)
    get_filename_component(DIR ${PATH} DIRECTORY)

    build_flatbuffers(${PATH}
        "${CMAKE_CURRENT_SOURCE_DIR}/${DIR};${CMAKE_SOURCE_DIR}/src"
        "${NAME_WE}-generated"
        ""
        "${CMAKE_CURRENT_BINARY_DIR}/${DIR}"
        ""
        ""
        )

      add_library(${NAME} INTERFACE)
      target_link_libraries(${NAME} INTERFACE common)
      target_include_directories(${NAME}
          INTERFACE
              $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
              ${PROJECT_SOURCE_DIR}
              ${PROJECT_BINARY_DIR}/src
              ${PROJECT_BINARY_DIR}
      )

    add_dependencies(${NAME} "${NAME_WE}-generated" ${FBS_DEPS})
endmacro()
