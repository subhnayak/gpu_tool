/*
 * decoder.cpp — Implementation of the table-driven decoder.
 */

#include "decoder.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace toyisa {

// ---- decode_one ----
// Try each table entry until (word & mask) == match_value.
// For a real ISA with hundreds of entries, you'd pre-sort the table by opcode
// and use a lookup array indexed by the opcode field, or build a trie/decision
// tree on the most-discriminating bits. For our ~30 entries, linear scan is fine.
DecodedInst decode_one(const std::vector<uint32_t>& words, size_t& index, uint32_t base_address) {
    DecodedInst inst{};
    inst.address = base_address + static_cast<uint32_t>(index * 4);
    inst.raw_word0 = words[index];
    inst.rd = -1;
    inst.rs1 = -1;
    inst.rs2 = -1;
    inst.immediate = 0;
    inst.is_two_word = false;
    inst.is_branch = false;
    inst.is_call = false;
    inst.is_return = false;
    inst.is_halt = false;
    inst.is_barrier = false;
    inst.is_unknown = false;
    inst.format = InstFormat::R;

    uint32_t w = words[index];

    // Extract predicate (common to all formats)
    inst.pred_negate = (extract_field(w, FIELD_PN) != 0);
    inst.pred_reg = static_cast<int>(extract_field(w, FIELD_PRED));

    // Search the opcode table
    const auto& table = get_opcode_table();
    const OpcodeEntry* matched = nullptr;
    for (const auto& entry : table) {
        if ((w & entry.mask) == entry.match_value) {
            matched = &entry;
            break;
        }
    }

    if (!matched) {
        // Unrecognized instruction — emit .unknown and advance 1 word.
        // This ensures forward progress on corrupt data.
        inst.mnemonic = ".unknown";
        inst.is_unknown = true;
        index++;
        return inst;
    }

    inst.mnemonic = matched->mnemonic;
    inst.format = matched->format;
    inst.is_branch = matched->is_branch;
    inst.is_call = matched->is_call;
    inst.is_return = matched->is_return;
    inst.is_halt = matched->is_halt;
    inst.is_barrier = matched->is_barrier;
    inst.is_two_word = matched->is_two_word;

    // Extract operands based on format
    switch (matched->format) {
        case InstFormat::R:
            inst.rd  = static_cast<int>(extract_field(w, FIELD_RD));
            inst.rs1 = static_cast<int>(extract_field(w, FIELD_RS1));
            inst.rs2 = static_cast<int>(extract_field(w, FIELD_RS2));
            break;

        case InstFormat::I: {
            inst.rd  = static_cast<int>(extract_field(w, FIELD_RD));
            inst.rs1 = static_cast<int>(extract_field(w, FIELD_RS1));
            uint32_t raw_imm = extract_field(w, FIELD_IMM12);
            if (matched->imm_is_unsigned) {
                inst.immediate = static_cast<int32_t>(raw_imm);  // zero-extended
            } else {
                inst.immediate = sign_extend(raw_imm, FIELD_IMM12.width);
            }
            break;
        }

        case InstFormat::B: {
            inst.rs1 = static_cast<int>(extract_field(w, FIELD_RS1_B));
            uint32_t raw_off = extract_field(w, FIELD_OFFSET17);
            inst.immediate = sign_extend(raw_off, FIELD_OFFSET17.width);
            break;
        }

        case InstFormat::L: {
            inst.rd = static_cast<int>(extract_field(w, FIELD_RD));
            // Two-word: need the second word
            if (index + 1 >= words.size()) {
                // No room for word2 — illegal encoding, treat as unknown
                inst.mnemonic = ".unknown";
                inst.is_unknown = true;
                inst.is_two_word = false;
                index++;
                return inst;
            }
            uint32_t word2 = words[index + 1];
            inst.raw_word1 = word2;
            inst.immediate = static_cast<int32_t>(word2);  // Full 32-bit immediate
            break;
        }
    }

    // Advance past consumed words
    index += matched->is_two_word ? 2 : 1;
    return inst;
}

// ---- decode_all ----
std::vector<DecodedInst> decode_all(const std::vector<uint32_t>& words, uint32_t base_address) {
    std::vector<DecodedInst> result;
    result.reserve(words.size());  // At most one inst per word
    size_t index = 0;
    while (index < words.size()) {
        result.push_back(decode_one(words, index, base_address));
    }
    return result;
}

// ---- format_inst ----
std::string format_inst(const DecodedInst& inst) {
    std::ostringstream os;
    // Address
    os << "0x" << std::hex << std::setfill('0') << std::setw(4) << inst.address << ":  ";

    // Predicate prefix
    if (inst.pred_reg != 0) {
        os << "@";
        if (inst.pred_negate) os << "!";
        os << "P" << std::dec << inst.pred_reg << " ";
    }

    // Mnemonic
    os << inst.mnemonic;

    if (inst.is_unknown) {
        os << " 0x" << std::hex << std::setfill('0') << std::setw(8) << inst.raw_word0;
        return os.str();
    }

    // Operands
    switch (inst.format) {
        case InstFormat::R:
            if (inst.mnemonic == "NOP" || inst.mnemonic == "HALT" || inst.mnemonic == "RET") {
                // No operands (or minimal)
                if (inst.mnemonic == "BAR") {
                    os << " R" << std::dec << inst.rs1;
                }
            } else if (inst.mnemonic.substr(0, 4) == "SETP") {
                // SETP: P(rd[2:0]), RS1, RS2
                os << " P" << std::dec << (inst.rd & 0x7) << ", R" << inst.rs1 << ", R" << inst.rs2;
            } else if (inst.mnemonic == "BAR") {
                os << " R" << std::dec << inst.rs1;
            } else {
                os << " R" << std::dec << inst.rd << ", R" << inst.rs1 << ", R" << inst.rs2;
            }
            break;

        case InstFormat::I:
            if (inst.mnemonic == "LD" || inst.mnemonic == "LDB") {
                os << " R" << std::dec << inst.rd << ", [R" << inst.rs1;
                if (inst.immediate != 0)
                    os << std::showpos << std::dec << inst.immediate;
                os << "]";
            } else if (inst.mnemonic == "ST" || inst.mnemonic == "STB") {
                os << " R" << std::dec << inst.rd << ", [R" << inst.rs1;
                if (inst.immediate != 0)
                    os << std::showpos << std::dec << inst.immediate;
                os << "]";
            } else {
                os << " R" << std::dec << inst.rd << ", R" << inst.rs1 << ", "
                   << std::dec << inst.immediate;
            }
            break;

        case InstFormat::B:
            if (inst.is_return) {
                // RET has no printed operands
            } else {
                if (inst.mnemonic != "BRA") {
                    os << " R" << std::dec << inst.rs1 << ",";
                }
                // Print target address
                os << " 0x" << std::hex << std::setfill('0') << std::setw(4) << inst.branch_target();
            }
            break;

        case InstFormat::L:
            os << " R" << std::dec << inst.rd << ", 0x" << std::hex << std::setfill('0')
               << std::setw(8) << static_cast<uint32_t>(inst.immediate);
            break;
    }

    return os.str();
}

} // namespace toyisa
