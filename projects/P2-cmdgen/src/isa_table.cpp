#include "isa_table.h"

#include <algorithm>
#include <cctype>

namespace cmdgen {
namespace {

constexpr std::array<OperandKind, kMaxOperandsPerInstruction> kinds(OperandKind a = OperandKind::Immediate,
                                                                    OperandKind b = OperandKind::Immediate,
                                                                    OperandKind c = OperandKind::Immediate,
                                                                    OperandKind d = OperandKind::Immediate) {
    return {a, b, c, d};
}

constexpr std::array<FieldSpec, kMaxFieldsPerInstruction> fields(FieldSpec a = {},
                                                                 FieldSpec b = {},
                                                                 FieldSpec c = {},
                                                                 FieldSpec d = {}) {
    return {a, b, c, d};
}

constexpr std::uint32_t archAOpcode(std::uint32_t value) {
    return value << 28U;
}

constexpr std::uint32_t archBOpcode(std::uint32_t value) {
    return 0x40U + (value * 3U);
}

} // namespace

// This is the single source of truth for instruction layout.
//
// Encoder and decoder both consume this table. That shared ownership is the key
// idea: if the bit positions move, both directions see the same new rule.
const std::array<InstructionEncodingRule, 20> kIsaTable = {{
    {Architecture::ARCH_A, Opcode::NOP, "nop", archAOpcode(0U), 0xF0000000U, 0, kinds(), 0, fields()},
    {Architecture::ARCH_A, Opcode::ADD, "add", archAOpcode(1U), 0xF0000000U, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},
    {Architecture::ARCH_A, Opcode::MUL, "mul", archAOpcode(2U), 0xF0000000U, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},
    {Architecture::ARCH_A, Opcode::FMA, "fma", archAOpcode(3U), 0xF0000000U, 4,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register, OperandKind::Register), 4,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4),
            makeField(OperandMember::RegisterIndex, 3, 12, 4))},
    {Architecture::ARCH_A, Opcode::LOAD, "load", archAOpcode(4U), 0xF0000000U, 2,
     kinds(OperandKind::Register, OperandKind::Memory), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::MemoryBaseRegister, 1, 20, 4),
            makeField(OperandMember::MemoryOffset, 1, 0, 12, true))},
    {Architecture::ARCH_A, Opcode::STORE, "store", archAOpcode(5U), 0xF0000000U, 2,
     kinds(OperandKind::Register, OperandKind::Memory), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::MemoryBaseRegister, 1, 20, 4),
            makeField(OperandMember::MemoryOffset, 1, 0, 12, true))},
    {Architecture::ARCH_A, Opcode::BRANCH, "branch", archAOpcode(6U), 0xF0000000U, 2,
     kinds(OperandKind::Predicate, OperandKind::Immediate), 2,
     fields(makeField(OperandMember::PredicateIndex, 0, 24, 4),
            makeField(OperandMember::ImmediateValue, 1, 0, 12, true))},
    {Architecture::ARCH_A, Opcode::CMP, "cmp", archAOpcode(7U), 0xF0000000U, 3,
     kinds(OperandKind::Predicate, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::PredicateIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},
    {Architecture::ARCH_A, Opcode::AND, "and", archAOpcode(8U), 0xF0000000U, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},
    {Architecture::ARCH_A, Opcode::OR, "or", archAOpcode(9U), 0xF0000000U, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 24, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},

    {Architecture::ARCH_B, Opcode::NOP, "nop", archBOpcode(0U), 0x000000FFU, 0, kinds(), 0, fields()},
    {Architecture::ARCH_B, Opcode::ADD, "add", archBOpcode(1U), 0x000000FFU, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 8, 4),
            makeField(OperandMember::RegisterIndex, 1, 12, 4),
            makeField(OperandMember::RegisterIndex, 2, 20, 4))},
    {Architecture::ARCH_B, Opcode::MUL, "mul", archBOpcode(2U), 0x000000FFU, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 20, 4),
            makeField(OperandMember::RegisterIndex, 1, 8, 4),
            makeField(OperandMember::RegisterIndex, 2, 12, 4))},
    {Architecture::ARCH_B, Opcode::FMA, "fma", archBOpcode(3U), 0x000000FFU, 4,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register, OperandKind::Register), 4,
     fields(makeField(OperandMember::RegisterIndex, 0, 8, 4),
            makeField(OperandMember::RegisterIndex, 1, 12, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4),
            makeField(OperandMember::RegisterIndex, 3, 24, 4))},
    {Architecture::ARCH_B, Opcode::LOAD, "load", archBOpcode(4U), 0x000000FFU, 2,
     kinds(OperandKind::Register, OperandKind::Memory), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 8, 4),
            makeField(OperandMember::MemoryBaseRegister, 1, 12, 4),
            makeField(OperandMember::MemoryOffset, 1, 20, 12, true))},
    {Architecture::ARCH_B, Opcode::STORE, "store", archBOpcode(5U), 0x000000FFU, 2,
     kinds(OperandKind::Register, OperandKind::Memory), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 8, 4),
            makeField(OperandMember::MemoryBaseRegister, 1, 12, 4),
            makeField(OperandMember::MemoryOffset, 1, 20, 12, true))},
    {Architecture::ARCH_B, Opcode::BRANCH, "branch", archBOpcode(6U), 0x000000FFU, 2,
     kinds(OperandKind::Predicate, OperandKind::Immediate), 2,
     fields(makeField(OperandMember::PredicateIndex, 0, 8, 4),
            makeField(OperandMember::ImmediateValue, 1, 20, 12, true))},
    {Architecture::ARCH_B, Opcode::CMP, "cmp", archBOpcode(7U), 0x000000FFU, 3,
     kinds(OperandKind::Predicate, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::PredicateIndex, 0, 8, 4),
            makeField(OperandMember::RegisterIndex, 1, 12, 4),
            makeField(OperandMember::RegisterIndex, 2, 16, 4))},
    {Architecture::ARCH_B, Opcode::AND, "and", archBOpcode(8U), 0x000000FFU, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 20, 4),
            makeField(OperandMember::RegisterIndex, 1, 8, 4),
            makeField(OperandMember::RegisterIndex, 2, 12, 4))},
    {Architecture::ARCH_B, Opcode::OR, "or", archBOpcode(9U), 0x000000FFU, 3,
     kinds(OperandKind::Register, OperandKind::Register, OperandKind::Register), 3,
     fields(makeField(OperandMember::RegisterIndex, 0, 8, 4),
            makeField(OperandMember::RegisterIndex, 1, 20, 4),
            makeField(OperandMember::RegisterIndex, 2, 12, 4))}
}};

const InstructionEncodingRule* findEncodingRule(Architecture architecture, Opcode opcode) {
    for (const InstructionEncodingRule& rule : kIsaTable) {
        if (rule.architecture == architecture && rule.opcode == opcode) {
            return &rule;
        }
    }
    return nullptr;
}

const InstructionEncodingRule* findDecodingRule(Architecture architecture, std::uint32_t encoded_word) {
    for (const InstructionEncodingRule& rule : kIsaTable) {
        if (rule.architecture == architecture && (encoded_word & rule.mask) == rule.match) {
            return &rule;
        }
    }
    return nullptr;
}

const char* architectureName(Architecture architecture) {
    switch (architecture) {
    case Architecture::ARCH_A: return "A";
    case Architecture::ARCH_B: return "B";
    }
    return "?";
}

bool parseArchitecture(const std::string& text, Architecture& architecture) {
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });

    if (upper == "A" || upper == "ARCH_A") {
        architecture = Architecture::ARCH_A;
        return true;
    }
    if (upper == "B" || upper == "ARCH_B") {
        architecture = Architecture::ARCH_B;
        return true;
    }
    return false;
}

} // namespace cmdgen
