/*
 * cfg.cpp — CFG reconstruction and loop detection.
 */

#include "cfg.h"
#include <sstream>
#include <algorithm>
#include <stack>
#include <iomanip>
#include <functional>

namespace toyisa {

// ---- CFG::build ----
CFG CFG::build(const std::vector<DecodedInst>& instructions) {
    CFG cfg;
    if (instructions.empty()) return cfg;

    cfg.entry_addr = instructions.front().address;

    // Step 1: Identify leaders
    std::set<uint32_t> leaders;
    leaders.insert(instructions.front().address);  // First instruction is always a leader

    // Build address-to-index map for fast lookup
    std::map<uint32_t, size_t> addr_to_idx;
    for (size_t i = 0; i < instructions.size(); i++) {
        addr_to_idx[instructions[i].address] = i;
    }

    for (size_t i = 0; i < instructions.size(); i++) {
        const auto& inst = instructions[i];
        if (inst.is_branch || inst.is_call) {
            // Branch target is a leader
            uint32_t target = inst.branch_target();
            if (addr_to_idx.count(target)) {
                leaders.insert(target);
            }
            // Instruction after branch is a leader (fallthrough)
            if (i + 1 < instructions.size()) {
                leaders.insert(instructions[i + 1].address);
            }
        }
        if (inst.is_halt || inst.is_return) {
            // Instruction after halt/ret is a leader
            if (i + 1 < instructions.size()) {
                leaders.insert(instructions[i + 1].address);
            }
        }
    }

    // Step 2: Build basic blocks
    BasicBlock* current_block = nullptr;
    for (const auto& inst : instructions) {
        if (leaders.count(inst.address)) {
            // Start a new block
            if (current_block) {
                current_block->end_addr = inst.address;
                cfg.blocks[current_block->start_addr] = *current_block;
            }
            BasicBlock bb;
            bb.start_addr = inst.address;
            bb.end_addr = inst.address;
            bb.is_entry = (inst.address == cfg.entry_addr);
            bb.is_exit = false;
            current_block = &cfg.blocks[inst.address];
            *current_block = bb;
        }
        if (current_block) {
            current_block->instructions.push_back(inst);
            current_block->end_addr = inst.address + inst.size();
        }
    }

    // Step 3: Add edges
    for (auto& [addr, block] : cfg.blocks) {
        if (block.instructions.empty()) continue;
        const auto& last = block.instructions.back();

        bool falls_through = true;

        if (last.is_halt || last.is_return) {
            block.is_exit = true;
            falls_through = false;
        }

        if (last.is_branch) {
            uint32_t target = last.branch_target();
            if (cfg.blocks.count(target)) {
                block.successors.push_back(target);
                cfg.blocks[target].predecessors.push_back(addr);
            }
            // Unconditional branch (BRA with no predicate, or predicate P0) doesn't fall through
            if (last.mnemonic == "BRA" && last.pred_reg == 0) {
                falls_through = false;
            }
        }

        if (falls_through) {
            uint32_t next_addr = block.end_addr;
            if (cfg.blocks.count(next_addr)) {
                block.successors.push_back(next_addr);
                cfg.blocks[next_addr].predecessors.push_back(addr);
            }
        }
    }

    cfg.detect_loops();
    return cfg;
}

// ---- Loop detection via DFS ----
// A back edge is an edge (u, v) where v dominates u (or equivalently,
// v is an ancestor of u in the DFS tree). This is a simplified detection
// using the "gray node" approach from CLRS.
void CFG::detect_loops() {
    enum Color { WHITE, GRAY, BLACK };
    std::map<uint32_t, Color> color;
    for (const auto& [addr, _] : blocks) color[addr] = WHITE;

    std::function<void(uint32_t)> dfs = [&](uint32_t u) {
        color[u] = GRAY;
        for (uint32_t v : blocks[u].successors) {
            if (color.count(v) == 0) continue;
            if (color[v] == GRAY) {
                // Back edge found — this indicates a loop
                back_edges.insert({u, v});
            } else if (color[v] == WHITE) {
                dfs(v);
            }
        }
        color[u] = BLACK;
    };

    if (blocks.count(entry_addr)) {
        dfs(entry_addr);
    }
}

// ---- Text output ----
std::string CFG::to_text() const {
    std::ostringstream os;
    os << "=== Control Flow Graph ===\n";
    os << "Entry: 0x" << std::hex << std::setfill('0') << std::setw(4) << entry_addr << "\n";
    os << "Blocks: " << std::dec << blocks.size() << "\n";

    if (!back_edges.empty()) {
        os << "Loops (back edges):\n";
        for (const auto& [src, dst] : back_edges) {
            os << "  0x" << std::hex << std::setw(4) << src
               << " -> 0x" << std::setw(4) << dst << "\n";
        }
    }
    os << "\n";

    for (const auto& [addr, block] : blocks) {
        os << "BB_0x" << std::hex << std::setfill('0') << std::setw(4) << addr;
        if (block.is_entry) os << " [ENTRY]";
        if (block.is_exit) os << " [EXIT]";
        os << ":\n";

        for (const auto& inst : block.instructions) {
            os << "  " << format_inst(inst) << "\n";
        }

        if (!block.successors.empty()) {
            os << "  -> ";
            for (size_t i = 0; i < block.successors.size(); i++) {
                if (i > 0) os << ", ";
                os << "BB_0x" << std::hex << std::setfill('0') << std::setw(4) << block.successors[i];
            }
            os << "\n";
        }
        os << "\n";
    }
    return os.str();
}

// ---- DOT output ----
std::string CFG::to_dot() const {
    std::ostringstream os;
    os << "digraph CFG {\n";
    os << "  rankdir=TB;\n";
    os << "  node [shape=box, fontname=\"Courier\"];\n\n";

    for (const auto& [addr, block] : blocks) {
        os << "  BB_0x" << std::hex << std::setfill('0') << std::setw(4) << addr;
        os << " [label=\"";
        for (const auto& inst : block.instructions) {
            std::string s = format_inst(inst);
            // Escape quotes for DOT
            for (char c : s) {
                if (c == '"') os << "\\\"";
                else if (c == '\\') os << "\\\\";
                else os << c;
            }
            os << "\\n";
        }
        os << "\"";
        if (block.is_entry) os << ", style=bold, color=green";
        if (block.is_exit) os << ", style=bold, color=red";
        os << "];\n";
    }

    os << "\n";
    for (const auto& [addr, block] : blocks) {
        for (uint32_t succ : block.successors) {
            os << "  BB_0x" << std::hex << std::setfill('0') << std::setw(4) << addr
               << " -> BB_0x" << std::setw(4) << succ;
            if (back_edges.count({addr, succ})) {
                os << " [style=dashed, color=blue, label=\"loop\"]";
            }
            os << ";\n";
        }
    }

    os << "}\n";
    return os.str();
}

// ---- Recursive-descent disassembly ----
// Follow control flow from entry points, only decoding reachable code.
// This avoids misinterpreting data-in-code as instructions.
std::vector<DecodedInst> recursive_descent(const std::vector<uint32_t>& words,
                                            const std::vector<uint32_t>& entry_points,
                                            uint32_t base_address) {
    std::set<uint32_t> visited;         // Addresses we've decoded
    std::stack<uint32_t> worklist;      // Addresses to explore
    std::map<uint32_t, DecodedInst> decoded;  // addr -> decoded instruction

    for (uint32_t ep : entry_points) {
        worklist.push(ep);
    }

    while (!worklist.empty()) {
        uint32_t addr = worklist.top();
        worklist.pop();

        if (visited.count(addr)) continue;

        // Convert byte address to word index
        uint32_t byte_off = addr - base_address;
        if (byte_off % 4 != 0) continue;
        size_t idx = byte_off / 4;
        if (idx >= words.size()) continue;

        visited.insert(addr);

        DecodedInst inst = decode_one(words, idx, base_address);
        decoded[addr] = inst;

        if (inst.is_unknown || inst.is_halt || inst.is_return) continue;

        // Add successors
        if (inst.is_branch) {
            uint32_t target = inst.branch_target();
            worklist.push(target);
            // Conditional branches also fall through
            if (inst.mnemonic != "BRA" || inst.pred_reg != 0) {
                worklist.push(addr + inst.size());
            }
        } else {
            // Fall through to next instruction
            worklist.push(addr + inst.size());
        }
    }

    // Collect in address order
    std::vector<DecodedInst> result;
    for (const auto& [a, inst] : decoded) {
        result.push_back(inst);
    }
    return result;
}

} // namespace toyisa
