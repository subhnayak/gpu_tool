/*
 * toydis_main.cpp — Disassembler CLI for ToyGPU ISA.
 *
 * Usage: toydis [--mode linear|recursive|both] [--cfg] [--dot output.dot] <input.bin>
 *
 * Modes:
 *   linear     — Linear sweep: decode every word in order (default)
 *   recursive  — Recursive descent: follow control flow from address 0
 *   both       — Run both and report differences (this is how you find
 *                data-in-code: the linear sweep decodes it as instructions,
 *                the recursive descent skips it)
 *
 * --cfg: print the control-flow graph
 * --dot FILE: write a Graphviz DOT file for the CFG
 *
 * Exit codes: 0 = success, 1 = error
 */

#include "decoder.h"
#include "cfg.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <set>

int main(int argc, char* argv[]) {
    std::string mode = "linear";
    std::string dot_file;
    bool show_cfg = false;
    std::string input_file;

    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--cfg") == 0) {
            show_cfg = true;
        } else if (std::strcmp(argv[i], "--dot") == 0 && i + 1 < argc) {
            dot_file = argv[++i];
            show_cfg = true;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (input_file.empty()) {
        std::cerr << "Usage: toydis [--mode linear|recursive|both] [--cfg] [--dot out.dot] <input.bin>\n";
        return 1;
    }

    // Read binary
    std::ifstream in(input_file, std::ios::binary);
    if (!in) {
        std::cerr << "Error: cannot open " << input_file << "\n";
        return 1;
    }
    std::vector<uint32_t> words;
    uint32_t w;
    while (in.read(reinterpret_cast<char*>(&w), 4)) {
        words.push_back(w);
    }
    in.close();

    if (words.empty()) {
        std::cerr << "Warning: empty binary\n";
        return 0;
    }

    // Decode
    std::vector<toyisa::DecodedInst> linear_insts;
    std::vector<toyisa::DecodedInst> recursive_insts;

    if (mode == "linear" || mode == "both") {
        linear_insts = toyisa::decode_all(words);
    }
    if (mode == "recursive" || mode == "both") {
        recursive_insts = toyisa::recursive_descent(words, {0});
    }

    // Print disassembly
    if (mode == "both") {
        // Compare the two approaches
        std::set<uint32_t> linear_addrs, recursive_addrs;
        for (const auto& inst : linear_insts) linear_addrs.insert(inst.address);
        for (const auto& inst : recursive_insts) recursive_addrs.insert(inst.address);

        std::cout << "=== Linear Sweep ===\n";
        for (const auto& inst : linear_insts) {
            std::string marker = recursive_addrs.count(inst.address) ? "  " : "* ";
            std::cout << marker << toyisa::format_inst(inst) << "\n";
        }

        std::cout << "\n=== Recursive Descent ===\n";
        for (const auto& inst : recursive_insts) {
            std::cout << "  " << toyisa::format_inst(inst) << "\n";
        }

        // Report differences
        std::set<uint32_t> only_linear, only_recursive;
        for (uint32_t a : linear_addrs) {
            if (!recursive_addrs.count(a)) only_linear.insert(a);
        }
        for (uint32_t a : recursive_addrs) {
            if (!linear_addrs.count(a)) only_recursive.insert(a);
        }

        if (!only_linear.empty()) {
            std::cout << "\n=== Data-in-code candidates (linear only) ===\n";
            std::cout << "These addresses were decoded by linear sweep but NOT reached\n";
            std::cout << "by recursive descent — they are likely embedded data:\n";
            for (uint32_t a : only_linear) {
                std::cout << "  0x" << std::hex << a << "\n";
            }
        }
    } else {
        const auto& insts = (mode == "recursive") ? recursive_insts : linear_insts;
        for (const auto& inst : insts) {
            std::cout << toyisa::format_inst(inst) << "\n";
        }
    }

    // CFG
    if (show_cfg) {
        const auto& insts = (mode == "recursive") ? recursive_insts :
                            (!linear_insts.empty() ? linear_insts : recursive_insts);
        auto cfg = toyisa::CFG::build(insts);
        std::cout << "\n" << cfg.to_text();

        if (!dot_file.empty()) {
            std::ofstream dot_out(dot_file);
            if (dot_out) {
                dot_out << cfg.to_dot();
                std::cout << "DOT file written to " << dot_file << "\n";
            } else {
                std::cerr << "Error: cannot write " << dot_file << "\n";
            }
        }
    }

    return 0;
}
