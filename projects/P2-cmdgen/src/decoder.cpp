#include "decoder.h"

#include <stdexcept>
#include <utility>

namespace cmdgen {
namespace {

std::uint32_t bitMask(std::uint8_t width) {
    if (width == 0) {
        return 0U;
    }
    if (width >= 32) {
        return 0xFFFFFFFFU;
    }
    return (1U << width) - 1U;
}

std::int32_t signExtend(std::uint32_t value, std::uint8_t width) {
    if (width == 0) {
        return 0;
    }
    if (width < 32 && (value & (1U << (width - 1U))) != 0U) {
        value |= ~bitMask(width);
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t extractField(std::uint32_t word, const FieldSpec& field) {
    const std::uint32_t raw = (word >> field.bit_offset) & bitMask(field.bit_width);
    if (field.is_signed) {
        return signExtend(raw, field.bit_width);
    }
    return static_cast<std::int32_t>(raw);
}

Operand makeZeroOperand(OperandKind kind) {
    switch (kind) {
    case OperandKind::Register:
        return Operand::makeRegister(0);
    case OperandKind::Immediate:
        return Operand::makeImmediate(0);
    case OperandKind::Memory:
        return Operand::makeMemory(0, 0);
    case OperandKind::Predicate:
        return Operand::makePredicate(0);
    }
    return Operand::makeImmediate(0);
}

void applyField(Operand& operand, OperandMember member, std::int32_t value) {
    switch (member) {
    case OperandMember::RegisterIndex:
        operand.reg = static_cast<std::uint32_t>(value);
        break;
    case OperandMember::ImmediateValue:
        operand.imm = value;
        break;
    case OperandMember::MemoryBaseRegister:
        operand.base_reg = static_cast<std::uint32_t>(value);
        break;
    case OperandMember::MemoryOffset:
        operand.offset = value;
        break;
    case OperandMember::PredicateIndex:
        operand.pred = static_cast<std::uint32_t>(value);
        break;
    case OperandMember::None:
        break;
    }
}

} // namespace

Instruction Decoder::decodeInstruction(Architecture architecture, std::uint32_t encoded_word) const {
    const InstructionEncodingRule* rule = findDecodingRule(architecture, encoded_word);
    if (rule == nullptr) {
        throw std::runtime_error("No decoding rule matched instruction word");
    }

    Instruction instruction;
    instruction.opcode = rule->opcode;
    instruction.operands.reserve(rule->operand_count);

    for (std::uint8_t i = 0; i < rule->operand_count; ++i) {
        instruction.operands.push_back(makeZeroOperand(rule->operand_kinds[i]));
    }

    for (std::uint8_t i = 0; i < rule->field_count; ++i) {
        const FieldSpec& field = rule->fields[i];
        const std::int32_t value = extractField(encoded_word, field);
        applyField(instruction.operands[field.operand_index], field.member, value);
    }

    return instruction;
}

Program Decoder::decodeProgram(Architecture architecture, const std::vector<std::uint32_t>& words) const {
    Program program;
    BasicBlock block;
    block.name = "decoded_entry";
    block.instructions.reserve(words.size());

    for (std::uint32_t word : words) {
        block.instructions.push_back(decodeInstruction(architecture, word));
    }

    program.blocks.push_back(std::move(block));
    return program;
}

} // namespace cmdgen
