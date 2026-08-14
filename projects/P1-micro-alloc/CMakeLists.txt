cmake_minimum_required(VERSION 3.16)
project(P1MicroAlloc VERSION 1.0 LANGUAGES CXX)

option(ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer on GCC/Clang" OFF)

include(CTest)

add_library(microalloc INTERFACE)
target_compile_features(microalloc INTERFACE cxx_std_17)
target_include_directories(microalloc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(microalloc INTERFACE
        /W4
        /permissive-
        /EHsc
        /Zc:__cplusplus
        /wd4127
    )
else()
    target_compile_options(microalloc INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wundef
        -Wold-style-cast
    )
endif()

add_executable(bench src/bench.cpp)
target_link_libraries(bench PRIVATE microalloc)

add_executable(tests src/tests.cpp)
target_link_libraries(tests PRIVATE microalloc)

if(ENABLE_ASAN)
    if(MSVC)
        message(WARNING "ENABLE_ASAN only adds flags automatically for GCC/Clang in this project.")
    else()
        foreach(target bench tests)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        endforeach()
    endif()
endif()

if(BUILD_TESTING)
    add_test(NAME microalloc_tests COMMAND tests)
endif()
