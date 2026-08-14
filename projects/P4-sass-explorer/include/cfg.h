/*
 * cfg.h — Control-Flow Graph reconstruction from decoded instructions.
 *
 * ===== INTERVIEW LESSON =====
 * "How do you reconstruct a CFG from a flat instruction stream?"
 * 1. Identify LEADERS: the first instruction, any branch target, any
 *    instruction following a branch/halt.
 * 2. Each leader starts a BASIC BLOCK that extends until the next leader
 *    or a terminating instruction.
 * 3. Add EDGES: fallthrough (if the block doesn't end in unconditional
 *    branch/halt/return) and taken (branch target).
 * 4. Mark back edges (target dominates source) as LOOP back-edges.
 *
 * "What is the difference between linear-sweep and recursive-descent
 *  disassembly, and why does it matter?"
 * Linear sweep: decode every word in order. Simple, but confused by
 *   data embedded in the code section.
 * Recursive descent: start at known entry points, follow control flow.
 *   Misses unreachable code but avoids data-as-code errors.
 * A real tool runs BOTH and reports disagreements — the differences
 * highlight data-in-code regions or padding.
 * =====
 */

#ifndef CFG_H
#define CFG_H

#include "decoder.h"
#include <vector>
#include <string>
#include <set>
#include <map>

namespace toyisa {

struct BasicBlock {
    uint32_t start_addr;       // Address of first instruction
    uint32_t end_addr;         // Address AFTER last instruction
    std::vector<DecodedInst> instructions;
    std::vector<uint32_t> successors;     // Addresses of successor blocks
    std::vector<uint32_t> predecessors;   // Addresses of predecessor blocks
    bool is_entry;
    bool is_exit;              // Ends with HALT, RET, or unreachable
};

struct CFG {
    std::map<uint32_t, BasicBlock> blocks;  // start_addr -> block
    uint32_t entry_addr;
    std::set<std::pair<uint32_t, uint32_t>> back_edges;  // (src, dst) loop back edges

    // Build CFG from decoded instruction list
    static CFG build(const std::vector<DecodedInst>& instructions);

    // Detect back edges via DFS (indicating loops)
    void detect_loops();

    // Emit human-readable text
    std::string to_text() const;

    // Emit Graphviz DOT
    std::string to_dot() const;
};

// Recursive-descent disassembly: given raw words, explore from entry_points,
// following control flow. Returns decoded instructions (only reachable ones).
std::vector<DecodedInst> recursive_descent(const std::vector<uint32_t>& words,
                                            const std::vector<uint32_t>& entry_points,
                                            uint32_t base_address = 0);

} // namespace toyisa

#endif // CFG_H
