cmake_minimum_required(VERSION 3.14)
project(P5-graphics-stimulus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ═══════════════════════════════════════════════════════════════════════
# DETERMINISM: Disable floating-point optimizations that break bit-identical output.
# See DETERMINISM.md for why this matters.
#
# -ffp-contract=off  : prevent FMA contraction (changes result slightly)
# /fp:strict         : MSVC equivalent
# Do NOT use -ffast-math / /fp:fast — it breaks determinism!
# ═══════════════════════════════════════════════════════════════════════
if(MSVC)
    add_compile_options(/fp:strict /W4)
else()
    add_compile_options(-ffp-contract=off -Wall -Wextra)
endif()

# ═══════════════════════════════════════════════════════════════════════
# Core software rasterizer library (ZERO external dependencies)
# ═══════════════════════════════════════════════════════════════════════
add_library(softras STATIC
    src/raster.cpp
    src/shaders.cpp
    src/golden_check.cpp
)
target_include_directories(softras PUBLIC include)

# ═══════════════════════════════════════════════════════════════════════
# Stimulus CLI — the main executable
# ═══════════════════════════════════════════════════════════════════════
add_executable(stimulus src/stimulus_main.cpp)
target_link_libraries(stimulus PRIVATE softras)

# ═══════════════════════════════════════════════════════════════════════
# Tests — dependency-free assertion tests
# ═══════════════════════════════════════════════════════════════════════
add_executable(tests src/tests.cpp)
target_link_libraries(tests PRIVATE softras)

# ═══════════════════════════════════════════════════════════════════════
# Optional OpenGL comparison path (OFF by default)
# ═══════════════════════════════════════════════════════════════════════
option(ENABLE_GL "Enable OpenGL comparison path (requires OpenGL + GLFW)" OFF)

if(ENABLE_GL)
    find_package(OpenGL)
    find_package(glfw3 CONFIG QUIET)
    if(OpenGL_FOUND AND glfw3_FOUND)
        message(STATUS "OpenGL path ENABLED — building gl_compare target")
        # add_executable(gl_compare src/gl_compare.cpp)
        # target_link_libraries(gl_compare PRIVATE softras OpenGL::GL glfw)
    else()
        message(STATUS "OpenGL path requested but GL/GLFW not found — skipping")
    endif()
endif()

# CTest support
enable_testing()
add_test(NAME softras_tests COMMAND tests)
