/*
 * assembler.h — Two-pass assembler for the ToyGPU ISA.
 *
 * ===== INTERVIEW LESSON =====
 * "Why two passes?" Because forward references (branches to labels defined
 * later) cannot be resolved until all label addresses are known. Pass 1
 * collects labels and instruction sizes. Pass 2 encodes with resolved offsets.
 * This is the simplest correct approach. Single-pass is possible with
 * backpatching but more complex and error-prone.
 * =====
 */

#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include "toy_isa.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace toyisa {

struct AsmError {
    int line;
    std::string message;
};

struct AssemblerResult {
    std::vector<uint32_t> words;          // Encoded binary
    std::vector<AsmError> errors;         // Errors with line numbers
    std::unordered_map<std::string, uint32_t> labels;  // Label -> byte address
    bool success() const { return errors.empty(); }
};

// Assemble source text into binary words.
// Returns encoded words and any errors.
AssemblerResult assemble(const std::string& source);

} // namespace toyisa

#endif // ASSEMBLER_H
