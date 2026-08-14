cmake_minimum_required(VERSION 3.18)
project(CUDALadder LANGUAGES CXX CUDA)

# Default to sm_89 (Ada / RTX 4000 Ada).  Override with -DCMAKE_CUDA_ARCHITECTURES=native
# or any other arch list.  'native' auto-detects (CMake 3.24+).
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 89)
endif()

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)

# Add line-info for profiling and print register/shared-mem usage at build time.
add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:--generate-line-info>)
add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-v>)

# Common include directory.
include_directories(${CMAKE_SOURCE_DIR})

# --- Rung executables ---
set(RUNGS
    01_vector_add
    02_coalescing
    03_transpose
    04_reduction
    05_scan
    06_histogram
    07_sgemm
    08_radix_sort
    09_streams
    10_occupancy
)

foreach(RUNG ${RUNGS})
    add_executable(${RUNG} ${RUNG}.cu)
    set_target_properties(${RUNG} PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
    )
endforeach()

# --- Optional cuBLAS for 07_sgemm ---
option(USE_CUBLAS "Link cuBLAS for SGEMM comparison" OFF)
if(USE_CUBLAS)
    find_package(CUDAToolkit REQUIRED)
    target_compile_definitions(07_sgemm PRIVATE USE_CUBLAS)
    target_link_libraries(07_sgemm PRIVATE CUDA::cublas)
endif()

# --- RUN_ALL custom target ---
add_custom_target(RUN_ALL
    COMMENT "Running all CUDA ladder rungs..."
)
foreach(RUNG ${RUNGS})
    add_custom_command(TARGET RUN_ALL POST_BUILD
        COMMAND ${RUNG}
        COMMENT "Running ${RUNG}..."
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_dependencies(RUN_ALL ${RUNG})
endforeach()
