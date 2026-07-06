# CUDA/GDR Support for 3FS
#
# This module provides optional CUDA support for GPU Direct RDMA (GDR).
# GDR enables direct data transfers between storage and GPU memory.
#
# Options:
#   HF3FS_ENABLE_GDR - Enable GPU Direct RDMA support (default: OFF)
#
# When HF3FS_ENABLE_GDR is ON:
#   - CUDA Toolkit will be searched
#   - HF3FS_GDR_ENABLED will be defined
#   - CUDA libraries will be linked to relevant targets
#
# Usage in CMakeLists.txt:
#   include(cmake/Cuda.cmake)
#   target_add_gdr_support(mytarget SCOPE PRIVATE)

option(HF3FS_ENABLE_GDR "Enable GPU Direct RDMA (GDR) support" OFF)

set(HF3FS_GDR_AVAILABLE OFF)
set(HF3FS_CUDA_LIBRARIES "")
set(HF3FS_CUDA_INCLUDE_DIRS "")

if(HF3FS_ENABLE_GDR)
    message(STATUS "GDR support requested, searching for CUDA...")

    # Try to find CUDA Toolkit
    # CMake 3.17+ has FindCUDAToolkit which is preferred
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.17")
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            set(HF3FS_GDR_AVAILABLE ON)
            set(HF3FS_CUDA_LIBRARIES CUDA::cudart)
            set(HF3FS_CUDA_INCLUDE_DIRS ${CUDAToolkit_INCLUDE_DIRS})
            message(STATUS "Found CUDA Toolkit ${CUDAToolkit_VERSION}")
            message(STATUS "  CUDA include: ${CUDAToolkit_INCLUDE_DIRS}")
            message(STATUS "  CUDA libraries: CUDA::cudart")
        endif()
    else()
        # Fallback for older CMake
        find_package(CUDA QUIET)
        if(CUDA_FOUND)
            set(HF3FS_GDR_AVAILABLE ON)
            set(HF3FS_CUDA_LIBRARIES ${CUDA_LIBRARIES})
            set(HF3FS_CUDA_INCLUDE_DIRS ${CUDA_INCLUDE_DIRS})
            message(STATUS "Found CUDA ${CUDA_VERSION}")
            message(STATUS "  CUDA include: ${CUDA_INCLUDE_DIRS}")
            message(STATUS "  CUDA libraries: ${CUDA_LIBRARIES}")
        endif()
    endif()

    if(HF3FS_GDR_AVAILABLE)
        message(STATUS "GDR support enabled")

        # Check for nvidia_peermem (informational only)
        if(EXISTS "/sys/module/nvidia_peermem")
            message(STATUS "nvidia_peermem module detected")
        else()
            message(STATUS "nvidia_peermem module not detected (required at runtime for GDR)")
        endif()
    else()
        message(FATAL_ERROR "HF3FS_ENABLE_GDR=ON requires CUDA Toolkit, but CUDA was not found")
    endif()
else()
    message(STATUS "GDR support disabled (use -DHF3FS_ENABLE_GDR=ON to enable)")
endif()

# Helper function to add GDR support to a target
function(target_add_gdr_support TARGET)
    cmake_parse_arguments(GDR "" "SCOPE" "" ${ARGN})
    if(NOT GDR_SCOPE)
        set(GDR_SCOPE PRIVATE)
    endif()

    if(HF3FS_GDR_AVAILABLE)
        target_compile_definitions(${TARGET} ${GDR_SCOPE} HF3FS_GDR_ENABLED)
        target_include_directories(${TARGET} PRIVATE ${HF3FS_CUDA_INCLUDE_DIRS})
        # Existing project helpers use the plain target_link_libraries signature.
        # Appending LINK_LIBRARIES keeps CUDA linkage private to this target.
        set_property(TARGET ${TARGET} APPEND PROPERTY LINK_LIBRARIES ${HF3FS_CUDA_LIBRARIES})
        message(STATUS "Added GDR support to target: ${TARGET} (${GDR_SCOPE})")
    endif()
endfunction()
