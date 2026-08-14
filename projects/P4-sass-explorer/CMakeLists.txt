cmake_minimum_required(VERSION 3.14)
project(ToyGPU_ISA_Tools CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Library: toyisa ----
# The core library containing the assembler, decoder, and CFG builder.
# Driven by the table in include/toy_isa.h.
add_library(toyisa
    src/decoder.cpp
    src/assembler.cpp
    src/cfg.cpp
)
target_include_directories(toyisa PUBLIC include)

# ---- CLI: toyasm (assembler) ----
add_executable(toyasm src/toyasm_main.cpp)
target_link_libraries(toyasm toyisa)

# ---- CLI: toydis (disassembler) ----
add_executable(toydis src/toydis_main.cpp)
target_link_libraries(toydis toyisa)

# ---- Tests ----
enable_testing()
add_executable(toyisa_tests src/tests.cpp)
target_link_libraries(toyisa_tests toyisa)
add_test(NAME toyisa_tests COMMAND toyisa_tests)
