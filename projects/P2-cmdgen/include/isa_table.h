#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ir.h"

namespace cmdgen {

// The fake architecture selector is deliberately outside the IR. The IR should
// be oblivious to target encodings; the architecture becomes relevant only when
// we choose a back-end or interpret a bit stream.
enum class Architecture {
    ARCH_A,
    ARCH_B
};

enum class OperandMember {
    None,
    RegisterIndex,
    ImmediateValue,
    MemoryBaseRegister,
    MemoryOffset,
    PredicateIndex
};

struct FieldSpec {
    OperandMember member = OperandMember::None;
    std::uint8_t operand_index = 0;
    std::uint8_t bit_offset = 0;
    std::uint8_t bit_width = 0;
    bool is_signed = false;
};

constexpr std::size_t kMaxOperandsPerInstruction = 4;
constexpr std::size_t kMaxFieldsPerInstruction = 4;

struct InstructionEncodingRule {
    Architecture architecture = Architecture::ARCH_A;
    Opcode opcode = Opcode::NOP;
    const char* mnemonic = "nop";
    std::uint32_t match = 0;
    std::uint32_t mask = 0;
    std::uint8_t operand_count = 0;
    std::array<OperandKind, kMaxOperandsPerInstruction> operand_kinds{};
    std::uint8_t field_count = 0;
    std::array<FieldSpec, kMaxFieldsPerInstruction> fields{};
};

constexpr FieldSpec makeField(OperandMember member,
                              std::uint8_t operand_index,
                              std::uint8_t bit_offset,
                              std::uint8_t bit_width,
                              bool is_signed = false) {
    return FieldSpec{member, operand_index, bit_offset, bit_width, is_signed};
}

extern const std::array<InstructionEncodingRule, 20> kIsaTable;

const InstructionEncodingRule* findEncodingRule(Architecture architecture, Opcode opcode);
const InstructionEncodingRule* findDecodingRule(Architecture architecture, std::uint32_t encoded_word);
const char* architectureName(Architecture architecture);
bool parseArchitecture(const std::string& text, Architecture& architecture);

} // namespace cmdgen
