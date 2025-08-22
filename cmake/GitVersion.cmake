find_package(Git REQUIRED)

if (NOT DEFINED PRE_CONFIGURE_DIR)
    set(PRE_CONFIGURE_DIR ${PROJECT_SOURCE_DIR}/src/common/utils)
endif ()

if (NOT DEFINED POST_BUILD_DIR)
    set(POST_BUILD_DIR ${PROJECT_BINARY_DIR})
endif ()

set(PRE_CONFIGURE_FILE ${PRE_CONFIGURE_DIR}/VersionInfo.cc.in)
set(POST_CONFIGURE_FILE ${POST_BUILD_DIR}/src/common/utils/VersionInfo.cc)

function(CheckGitWrite git_hash)
    file(WRITE ${POST_BUILD_DIR}/git-state.txt ${git_hash})
endfunction()

function(CheckGitRead git_hash)
    if (EXISTS ${POST_BUILD_DIR}/git-state.txt)
        file(STRINGS ${POST_BUILD_DIR}/git-state.txt CONTENT)
        LIST(GET CONTENT 0 var)
        set(${git_hash} ${var} PARENT_SCOPE)
    endif ()
endfunction()

function(CheckGitVersion)
    # Get the latest abbreviated commit hash of the working branch
    execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_COMMIT_HASH_SHORT
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_COMMIT_HASH_FULL
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%at --date=local
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_TIMESTAMP
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND date -d @${BUILD_TIMESTAMP} +%Y%m%d
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_DATE
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
            COMMAND bash -c "${GIT_EXECUTABLE} describe --tags --long | sed -E 's/(.*)-([0-9]+)-(\\w+)/\\2/g'"
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE BUILD_TAG_SEQ_NUM
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT DEFINED BUILD_TAG OR BUILD_TAG STREQUAL "")
        set(BUILD_TAG "250228")
    endif()
    if(NOT DEFINED BUILD_TAG_SEQ_NUM OR BUILD_TAG_SEQ_NUM STREQUAL "")
        set(BUILD_TAG_SEQ_NUM "1")
    endif()
    message(STATUS "Git Commit hash: ${BUILD_COMMIT_HASH_SHORT} ${BUILD_COMMIT_HASH_FULL}")
    message(STATUS "Git Commit Date & Timestamp: ${BUILD_DATE} ${BUILD_TIMESTAMP}")
    message(STATUS "Git Commit Tag & Seq Num: ${BUILD_TAG} ${BUILD_TAG_SEQ_NUM}")

    set(BUILD_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(BUILD_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(BUILD_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    set(BUILD_VERSION "${PROJECT_VERSION}")

    CheckGitRead(GIT_HASH_CACHE)

    if (NOT DEFINED GIT_HASH_CACHE)
        set(GIT_HASH_CACHE "INVALID")
    endif ()

    if (NOT DEFINED BUILD_ON_RELEASE_BRANCH)
      set(BUILD_ON_RELEASE_BRANCH "false")
    endif ()

    if (NOT DEFINED BUILD_PIPELINE_ID)
        set(BUILD_PIPELINE_ID "999999")
    endif()

    # Only update the git_version.cpp if the hash has changed. This will
    # prevent us from rebuilding the project more than we need to.
    if (NOT ${BUILD_COMMIT_HASH_FULL} STREQUAL ${GIT_HASH_CACHE} OR NOT EXISTS ${POST_CONFIGURE_FILE})
        # Set che GIT_HASH_CACHE variable the next build won't have
        # to regenerate the source file.
        CheckGitWrite("${BUILD_COMMIT_HASH_FULL}")
        configure_file(${PRE_CONFIGURE_FILE} ${POST_CONFIGURE_FILE} @ONLY)
    endif ()
endfunction()

function(CheckGitSetup project_src_dir)
    add_custom_target(AlwaysCheckGit COMMAND ${CMAKE_COMMAND}
        -DRUN_CHECK_GIT_VERSION=1
        -DPRE_CONFIGURE_DIR=${PRE_CONFIGURE_DIR}
        -DPOST_BUILD_DIR=${POST_BUILD_DIR}
        -DGIT_HASH_CACHE=${GIT_HASH_CACHE}
        -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DPROJECT_SOURCE_DIR=${project_src_dir}
        -P ${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake
        DEPENDS ${PRE_CONFIGURE_FILE}
        BYPRODUCTS ${POST_CONFIGURE_FILE}
    )

    add_library(version-info STATIC ${POST_CONFIGURE_FILE})
    target_include_directories(version-info PUBLIC ${PROJECT_SOURCE_DIR}/src)
    add_dependencies(version-info AlwaysCheckGit)
    CheckGitVersion()
endfunction()

if (RUN_CHECK_GIT_VERSION)
    CheckGitVersion()
endif ()
