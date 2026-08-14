cmake_minimum_required(VERSION 3.16)
project(P2_cmdgen LANGUAGES CXX)

# The sample is intentionally strict: the project is meant to teach clean,
# portable C++17 code that compiles under major compilers.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
enable_testing()

add_library(cmdgen_core STATIC
    src/isa_table.cpp
    src/encoder.cpp
    src/decoder.cpp
    src/random_gen.cpp
)

target_include_directories(cmdgen_core PUBLIC include)

if(MSVC)
    target_compile_options(cmdgen_core PRIVATE
        /W4
        /permissive-
        /EHsc
        /Zc:__cplusplus
    )
else()
    target_compile_options(cmdgen_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
    )
endif()

add_executable(cmdgen src/main.cpp)
target_link_libraries(cmdgen PRIVATE cmdgen_core)
target_include_directories(cmdgen PRIVATE include)
if(MSVC)
    target_compile_options(cmdgen PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
    target_compile_options(cmdgen PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
    )
endif()

add_executable(cmdgen_tests src/tests.cpp)
target_link_libraries(cmdgen_tests PRIVATE cmdgen_core)
target_include_directories(cmdgen_tests PRIVATE include)
if(MSVC)
    target_compile_options(cmdgen_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
    target_compile_options(cmdgen_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
    )
endif()

add_test(NAME cmdgen_tests COMMAND cmdgen_tests)
