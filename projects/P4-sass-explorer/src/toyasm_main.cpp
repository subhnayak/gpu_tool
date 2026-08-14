/*
 * toyasm_main.cpp — Assembler CLI for ToyGPU ISA.
 *
 * Usage: toyasm <input.asm> <output.bin>
 * Reads assembly text, encodes to binary, writes raw 32-bit words.
 * Exit code 0 on success, 1 on errors.
 */

#include "assembler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: toyasm <input.asm> <output.bin>\n";
        return 1;
    }

    // Read input file
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "Error: cannot open " << argv[1] << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();
    in.close();

    // Assemble
    auto result = toyisa::assemble(source);

    if (!result.success()) {
        for (const auto& err : result.errors) {
            std::cerr << argv[1] << ":" << err.line << ": error: " << err.message << "\n";
        }
        return 1;
    }

    // Write binary output (raw 32-bit words, little-endian)
    std::ofstream out(argv[2], std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot create " << argv[2] << "\n";
        return 1;
    }
    for (uint32_t w : result.words) {
        out.write(reinterpret_cast<const char*>(&w), 4);
    }
    out.close();

    std::cout << "Assembled " << result.words.size() << " words ("
              << result.words.size() * 4 << " bytes)\n";
    if (!result.labels.empty()) {
        std::cout << "Labels:\n";
        for (const auto& [name, addr] : result.labels) {
            std::cout << "  " << name << " = 0x" << std::hex << addr << "\n";
        }
    }
    return 0;
}
