# clang-tidy generates too many warnings, so just disable it by default.
option(ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)

find_program(CLANG_TIDY NAMES clang-tidy-14)
if(CLANG_TIDY)
    if(CMake_SOURCE_DIR STREQUAL CMake_BINARY_DIR)
        message(FATAL_ERROR "CMake_RUN_CLANG_TIDY requires an out-of-source build!")
    endif()

    if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
        message(WARNING "CMAKE_EXPORT_COMPILE_COMMANDS=OFF, clang-tidy may not works!!!")
    endif()

    set(HEADER_FILTER "${CMAKE_SOURCE_DIR}/\\(src\\|tests\\|benchmarks\\|demos\\)")

    if(ENABLE_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY} --header-filter ${HEADER_FILTER})

        # Create a preprocessor definition that depends on .clang-tidy content so
        # the compile command will change when .clang-tidy changes.  This ensures
        # that a subsequent build re-runs clang-tidy on all sources even if they
        # do not otherwise need to be recompiled.  Nothing actually uses this
        # definition.  We add it to targets on which we run clang-tidy just to
        # get the build dependency on the .clang-tidy file.
        file(SHA1 ${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy clang_tidy_sha1)
        set(CLANG_TIDY_DEFINITIONS "CLANG_TIDY_SHA1=${clang_tidy_sha1}")
        unset(clang_tidy_sha1)

        configure_file(.clang-tidy .clang-tidy COPYONLY)
    endif()

    set(SOURCE_DIRS
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/demos
        ${CMAKE_SOURCE_DIR}/benchmarks
    )

    # For now, it just hard codes the source files list to globs. That works
    # fine until we have another directory in `src/`. We should ideally gather
    # this from SOURCE_FILES list. But, should filter the thirs_party sources.
    # Taking a quick route for now. We should deal with it sometime down the line.
    add_custom_target(clang-tidy
        COMMENT "Running clang-tidy"
        COMMAND run-clang-tidy-14 -header-filter ${HEADER_FILTER} `find ${SOURCE_DIRS} -name "*.cc" -o -name "*.cpp" -not -name "*.actor.cpp" ` -quiet)

    add_custom_target(clang-tidy-fix
        COMMENT "Running clang-tidy -fix"
        COMMAND run-clang-tidy-14 -header-filter ${HEADER_FILTER} `find ${SOURCE_DIRS} -name "*.cc" -o -name "*.cpp" -not -name "*.actor.cpp" ` -fix -quiet)
else()
    message(WARNING "clang-tidy-14 not found!!!")
endif()