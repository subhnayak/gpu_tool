/*
 * toy_isa.h — The single source of truth for the ToyGPU ISA.
 *
 * ===== INTERVIEW LESSON =====
 * In a real GPU tools team, the ISA table is THE critical data structure.
 * Both the assembler and disassembler are driven from it. If they used
 * independent switch statements, they would inevitably drift apart.
 * A table-driven design means:
 *   - Adding an instruction = adding ONE row.
 *   - Correctness is verified by round-tripping: assemble then disassemble.
 *   - The table can be auto-generated from a machine-readable spec.
 * This directly answers: "How do you ensure encoder/decoder consistency?"
 * =====
 *
 * All constants, field specs, and opcode entries live here so that
 * toy_isa.h is the ONLY file that changes when the ISA evolves.
 */

#ifndef TOY_ISA_H
#define TOY_ISA_H

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace toyisa {

// ---- Instruction Formats ----
enum class InstFormat {
    R,  // Register-Register ALU: opcode(6) + rd(5) + rs1(5) + rs2(5) + func(3) + rsvd(4)
    I,  // Immediate:             opcode(6) + rd(5) + rs1(5) + imm12(12, signed)
    B,  // Branch:                opcode(6) + rs1(5) + offset17(17, signed words)
    L,  // Long immediate:        opcode(6) + rd(5) + imm_hi(17) [+ 32-bit word2]
};

// ---- Field Specification ----
// Each field is defined by its bit offset (LSB position), width, and signedness.
// The decoder extracts: (word >> offset) & ((1 << width) - 1), then sign-extends
// if is_signed.
struct FieldSpec {
    int offset;     // LSB bit position (0 = rightmost)
    int width;      // Number of bits
    bool is_signed; // Whether to sign-extend after extraction
};

// ---- Standard field positions (shared across formats) ----
// Predicate: bits 31 (negate) and 30:28 (register)
constexpr FieldSpec FIELD_PN   = {31, 1, false};
constexpr FieldSpec FIELD_PRED = {28, 3, false};
// Opcode: bits 27:22
constexpr FieldSpec FIELD_OPCODE = {22, 6, false};

// Format R fields
constexpr FieldSpec FIELD_RD   = {17, 5, false};
constexpr FieldSpec FIELD_RS1  = {12, 5, false};
constexpr FieldSpec FIELD_RS2  = {7, 5, false};
constexpr FieldSpec FIELD_FUNC = {4, 3, false};

// Format I fields (RD, RS1 same as R)
constexpr FieldSpec FIELD_IMM12 = {0, 12, true};  // Signed by default; ANDI/ORI override

// Format B fields
constexpr FieldSpec FIELD_RS1_B   = {17, 5, false};  // Same position as RD in R/I
constexpr FieldSpec FIELD_OFFSET17 = {0, 17, true};  // Signed, in words

// Format L fields (RD same as R)
constexpr FieldSpec FIELD_IMM_HI = {0, 17, false};  // Upper part (unsigned bits)

// ---- Helper: extract a field from a 32-bit word ----
inline uint32_t extract_field(uint32_t word, const FieldSpec& f) {
    uint32_t raw = (word >> f.offset) & ((1u << f.width) - 1);
    return raw;
}

// ---- Helper: sign-extend a value of given bit-width to 32 bits ----
// Interview question: "Why does sign extension matter for disassemblers?"
// Answer: Immediates and offsets are stored in fewer bits than the register
// width. A -1 stored in 12 bits is 0xFFF; without sign extension you'd print
// 4095 instead of -1, making the disassembly wrong and the tool useless.
inline int32_t sign_extend(uint32_t value, int width) {
    uint32_t sign_bit = 1u << (width - 1);
    // If sign bit set, OR in the upper bits
    if (value & sign_bit) {
        return static_cast<int32_t>(value | (~0u << width));
    }
    return static_cast<int32_t>(value);
}

// ---- Helper: encode a field into a 32-bit word ----
inline void encode_field(uint32_t& word, const FieldSpec& f, uint32_t value) {
    uint32_t mask = (1u << f.width) - 1;
    word |= (value & mask) << f.offset;
}

// ---- Opcode Entries ----
// This is the master table. Each entry defines everything needed to
// assemble AND disassemble one instruction (or one opcode+func pair).
//
// TO ADD A NEW INSTRUCTION: Add one entry here. That's it.
// The assembler will recognize the mnemonic, the disassembler will decode it,
// and round-trip tests will cover it automatically.

struct OpcodeEntry {
    std::string mnemonic;      // e.g. "ADD"
    uint32_t opcode;           // 6-bit opcode value
    uint32_t func;             // 3-bit func value (only for Format R; 0 otherwise)
    InstFormat format;         // Which format this instruction uses
    bool is_two_word;          // True for Format L (64-bit instructions)

    // For matching during decode: we compare (word & mask) == match_value
    uint32_t match_value;      // Expected bits after masking
    uint32_t mask;             // Bits to compare

    // Special flags
    bool imm_is_unsigned;      // For ANDI/ORI: don't sign-extend the immediate
    bool is_branch;            // Affects CFG construction
    bool is_call;              // Links return address
    bool is_return;            // Returns (indirect jump via R31)
    bool is_halt;              // Stops execution
    bool is_barrier;           // Synchronization
    bool sets_predicate;       // SETP: writes a predicate register
};

// Helper to build match/mask for Format R (opcode + func)
constexpr uint32_t R_MATCH(uint32_t op, uint32_t fn) {
    return (op << 22) | (fn << 4);
}
constexpr uint32_t R_MASK() {
    return (0x3Fu << 22) | (0x7u << 4);  // opcode and func bits
}

// Helper for other formats (opcode only)
constexpr uint32_t OP_MATCH(uint32_t op) {
    return op << 22;
}
constexpr uint32_t OP_MASK() {
    return 0x3Fu << 22;
}

// ---- THE TABLE ----
// This function returns the master opcode table. It is intentionally a
// function returning a const reference to a static vector so it is
// initialized once and shared.
inline const std::vector<OpcodeEntry>& get_opcode_table() {
    // Each entry:
    // {mnemonic, opcode, func, format, is_two_word,
    //  match_value, mask,
    //  imm_unsigned, is_branch, is_call, is_return, is_halt, is_barrier, sets_pred}

    static const std::vector<OpcodeEntry> table = {
        // Format R ALU ops (opcode=0x00, func varies)
        {"ADD",  0x00, 0, InstFormat::R, false, R_MATCH(0x00,0), R_MASK(), false,false,false,false,false,false,false},
        {"SUB",  0x00, 1, InstFormat::R, false, R_MATCH(0x00,1), R_MASK(), false,false,false,false,false,false,false},
        {"AND",  0x00, 2, InstFormat::R, false, R_MATCH(0x00,2), R_MASK(), false,false,false,false,false,false,false},
        {"OR",   0x00, 3, InstFormat::R, false, R_MATCH(0x00,3), R_MASK(), false,false,false,false,false,false,false},
        {"XOR",  0x00, 4, InstFormat::R, false, R_MATCH(0x00,4), R_MASK(), false,false,false,false,false,false,false},
        {"SHL",  0x00, 5, InstFormat::R, false, R_MATCH(0x00,5), R_MASK(), false,false,false,false,false,false,false},
        {"SHR",  0x00, 6, InstFormat::R, false, R_MATCH(0x00,6), R_MASK(), false,false,false,false,false,false,false},
        {"SLT",  0x00, 7, InstFormat::R, false, R_MATCH(0x00,7), R_MASK(), false,false,false,false,false,false,false},

        // SETP (opcode=0x01) — sets predicate registers
        // func encodes comparison: 0=EQ, 1=NE, 2=LT, 3=GE
        {"SETP.EQ", 0x01, 0, InstFormat::R, false, R_MATCH(0x01,0), R_MASK(), false,false,false,false,false,false,true},
        {"SETP.NE", 0x01, 1, InstFormat::R, false, R_MATCH(0x01,1), R_MASK(), false,false,false,false,false,false,true},
        {"SETP.LT", 0x01, 2, InstFormat::R, false, R_MATCH(0x01,2), R_MASK(), false,false,false,false,false,false,true},
        {"SETP.GE", 0x01, 3, InstFormat::R, false, R_MATCH(0x01,3), R_MASK(), false,false,false,false,false,false,true},

        // MUL (opcode=0x02)
        {"MUL",  0x02, 0, InstFormat::R, false, R_MATCH(0x02,0), R_MASK(), false,false,false,false,false,false,false},

        // Format I — immediate ops
        {"ADDI", 0x08, 0, InstFormat::I, false, OP_MATCH(0x08), OP_MASK(), false,false,false,false,false,false,false},
        {"ANDI", 0x09, 0, InstFormat::I, false, OP_MATCH(0x09), OP_MASK(), true, false,false,false,false,false,false},
        {"ORI",  0x0A, 0, InstFormat::I, false, OP_MATCH(0x0A), OP_MASK(), true, false,false,false,false,false,false},
        {"SLTI", 0x0B, 0, InstFormat::I, false, OP_MATCH(0x0B), OP_MASK(), false,false,false,false,false,false,false},

        // Load/Store (Format I)
        {"LD",   0x10, 0, InstFormat::I, false, OP_MATCH(0x10), OP_MASK(), false,false,false,false,false,false,false},
        {"ST",   0x11, 0, InstFormat::I, false, OP_MATCH(0x11), OP_MASK(), false,false,false,false,false,false,false},
        {"LDB",  0x12, 0, InstFormat::I, false, OP_MATCH(0x12), OP_MASK(), false,false,false,false,false,false,false},
        {"STB",  0x13, 0, InstFormat::I, false, OP_MATCH(0x13), OP_MASK(), false,false,false,false,false,false,false},

        // Format B — branches
        {"BRA",  0x18, 0, InstFormat::B, false, OP_MATCH(0x18), OP_MASK(), false,true, false,false,false,false,false},
        {"BEZ",  0x19, 0, InstFormat::B, false, OP_MATCH(0x19), OP_MASK(), false,true, false,false,false,false,false},
        {"BNZ",  0x1A, 0, InstFormat::B, false, OP_MATCH(0x1A), OP_MASK(), false,true, false,false,false,false,false},
        {"CALL", 0x1B, 0, InstFormat::B, false, OP_MATCH(0x1B), OP_MASK(), false,true, true, false,false,false,false},
        {"RET",  0x1C, 0, InstFormat::B, false, OP_MATCH(0x1C), OP_MASK(), false,false,false,true, false,false,false},

        // Format L — two-word
        {"LIMM", 0x20, 0, InstFormat::L, true,  OP_MATCH(0x20), OP_MASK(), false,false,false,false,false,false,false},

        // Special
        {"NOP",  0x3F, 0, InstFormat::R, false, R_MATCH(0x3F,0), R_MASK(), false,false,false,false,false,false,false},
        {"HALT", 0x3E, 0, InstFormat::R, false, R_MATCH(0x3E,0), R_MASK(), false,false,false,false,true, false,false},
        {"BAR",  0x3D, 0, InstFormat::R, false, R_MATCH(0x3D,0), R_MASK(), false,false,false,false,false,true, false},
    };
    return table;
}

// ---- Decoded Instruction ----
// This is the IR (intermediate representation) that the decoder produces
// and the CFG builder consumes. It is format-independent.
struct DecodedInst {
    uint32_t address;          // Byte address in the binary
    std::string mnemonic;      // e.g. "ADD", ".unknown"
    int pred_reg;              // Predicate register (0 = always)
    bool pred_negate;          // Negate predicate
    int rd;                    // Destination register (-1 if unused)
    int rs1;                   // Source register 1 (-1 if unused)
    int rs2;                   // Source register 2 (-1 if unused)
    int32_t immediate;         // Immediate/offset value
    uint32_t raw_word0;        // Original encoding (for hex dump)
    uint32_t raw_word1;        // Second word if two-word; 0 otherwise
    bool is_two_word;
    bool is_branch;
    bool is_call;
    bool is_return;
    bool is_halt;
    bool is_barrier;
    bool is_unknown;           // True if decoding failed
    InstFormat format;

    // Compute absolute branch target (byte address)
    uint32_t branch_target() const {
        // offset is in words; target = PC + 4 + offset*4
        return address + 4 + static_cast<uint32_t>(immediate * 4);
    }

    // Size in bytes (4 or 8)
    uint32_t size() const { return is_two_word ? 8 : 4; }
};

} // namespace toyisa

#endif // TOY_ISA_H
