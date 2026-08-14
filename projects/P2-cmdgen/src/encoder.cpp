#include "encoder.h"

#include <sstream>
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

bool fitsUnsigned(std::int32_t value, std::uint8_t width) {
    if (value < 0) {
        return false;
    }
    const std::uint32_t max_value = bitMask(width);
    return static_cast<std::uint32_t>(value) <= max_value;
}

bool fitsSigned(std::int32_t value, std::uint8_t width) {
    if (width == 0) {
        return value == 0;
    }
    const std::int32_t min_value = -(1 << (width - 1));
    const std::int32_t max_value = (1 << (width - 1)) - 1;
    return value >= min_value && value <= max_value;
}

void validateOperandShape(const Instruction& instruction, const InstructionEncodingRule& rule) {
    if (instruction.operands.size() != rule.operand_count) {
        std::ostringstream message;
        message << "Opcode " << opcodeName(instruction.opcode) << " expected "
                << static_cast<int>(rule.operand_count) << " operands but saw "
                << instruction.operands.size();
        throw std::runtime_error(message.str());
    }

    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
        if (instruction.operands[i].kind != rule.operand_kinds[i]) {
            std::ostringstream message;
            message << "Operand kind mismatch at index " << i << " for opcode "
                    << opcodeName(instruction.opcode);
            throw std::runtime_error(message.str());
        }
    }
}

std::int32_t extractOperandMember(const Operand& operand, OperandMember member) {
    switch (member) {
    case OperandMember::RegisterIndex:
        return static_cast<std::int32_t>(operand.reg);
    case OperandMember::ImmediateValue:
        return operand.imm;
    case OperandMember::MemoryBaseRegister:
        return static_cast<std::int32_t>(operand.base_reg);
    case OperandMember::MemoryOffset:
        return operand.offset;
    case OperandMember::PredicateIndex:
        return static_cast<std::int32_t>(operand.pred);
    case OperandMember::None:
        break;
    }
    return 0;
}

std::uint32_t encodeFieldValue(std::int32_t value, const FieldSpec& field) {
    const std::uint32_t mask = bitMask(field.bit_width);
    if (field.is_signed) {
        if (!fitsSigned(value, field.bit_width)) {
            throw std::runtime_error("Signed field overflow while encoding");
        }
        return static_cast<std::uint32_t>(value) & mask;
    }

    if (!fitsUnsigned(value, field.bit_width)) {
        throw std::runtime_error("Unsigned field overflow while encoding");
    }
    return static_cast<std::uint32_t>(value) & mask;
}

class TableDrivenEncoder : public Encoder {
public:
    TableDrivenEncoder(Architecture architecture, std::string name)
        : architecture_(architecture), name_(std::move(name)) {}

    Architecture architecture() const override {
        return architecture_;
    }

    std::string name() const override {
        return name_;
    }

    std::uint32_t encodeInstruction(const Instruction& instruction) const override {
        const InstructionEncodingRule* rule = findEncodingRule(architecture_, instruction.opcode);
        if (rule == nullptr) {
            throw std::runtime_error("No encoding rule for opcode");
        }

        validateOperandShape(instruction, *rule);

        std::uint32_t encoded = rule->match;
        for (std::uint8_t i = 0; i < rule->field_count; ++i) {
            const FieldSpec& field = rule->fields[i];
            const Operand& operand = instruction.operands[field.operand_index];
            const std::int32_t value = extractOperandMember(operand, field.member);
            const std::uint32_t encoded_value = encodeFieldValue(value, field);
            encoded |= (encoded_value << field.bit_offset);
        }
        return encoded;
    }

private:
    Architecture architecture_;
    std::string name_;
};

class ArchAEncoder final : public TableDrivenEncoder {
public:
    ArchAEncoder() : TableDrivenEncoder(Architecture::ARCH_A, "A") {}
};

class ArchBEncoder final : public TableDrivenEncoder {
public:
    ArchBEncoder() : TableDrivenEncoder(Architecture::ARCH_B, "B") {}
};

REGISTER_ENCODER(ArchAEncoder, "A");
REGISTER_ENCODER(ArchBEncoder, "B");

} // namespace

std::vector<std::uint32_t> Encoder::encodeProgram(const Program& program) const {
    std::vector<std::uint32_t> encoded;
    encoded.reserve(instructionCount(program));
    for (const BasicBlock& block : program.blocks) {
        for (const Instruction& instruction : block.instructions) {
            encoded.push_back(encodeInstruction(instruction));
        }
    }
    return encoded;
}

EncoderRegistry& EncoderRegistry::instance() {
    static EncoderRegistry registry;
    return registry;
}

void EncoderRegistry::registerFactory(const std::string& key, Factory factory) {
    factories_[key] = std::move(factory);
}

std::unique_ptr<Encoder> EncoderRegistry::create(const std::string& key) const {
    const auto it = factories_.find(key);
    if (it == factories_.end()) {
        return nullptr;
    }
    return (it->second)();
}

std::vector<std::string> EncoderRegistry::names() const {
    std::vector<std::string> keys;
    keys.reserve(factories_.size());
    for (const auto& entry : factories_) {
        keys.push_back(entry.first);
    }
    return keys;
}

EncoderRegistrar::EncoderRegistrar(const std::string& key, EncoderRegistry::Factory factory) {
    EncoderRegistry::instance().registerFactory(key, std::move(factory));
}

} // namespace cmdgen
