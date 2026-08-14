#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cmdgen {

// The IR is intentionally architecture-neutral.
//
// That means this file knows about operations such as ADD or LOAD, but it does
// not know where those operands live inside a 32-bit instruction word. The bit
// layout is a back-end concern, not an IR concern. Keeping the IR clean makes
// it reusable for multiple encodings, analyses, printers, and generators.
enum class Opcode {
    NOP,
    ADD,
    MUL,
    FMA,
    LOAD,
    STORE,
    BRANCH,
    CMP,
    AND,
    OR
};

inline constexpr std::array<Opcode, 10> allOpcodes() {
    return {Opcode::NOP, Opcode::ADD, Opcode::MUL, Opcode::FMA, Opcode::LOAD,
            Opcode::STORE, Opcode::BRANCH, Opcode::CMP, Opcode::AND, Opcode::OR};
}

inline const char* opcodeName(Opcode opcode) {
    switch (opcode) {
    case Opcode::NOP: return "nop";
    case Opcode::ADD: return "add";
    case Opcode::MUL: return "mul";
    case Opcode::FMA: return "fma";
    case Opcode::LOAD: return "load";
    case Opcode::STORE: return "store";
    case Opcode::BRANCH: return "branch";
    case Opcode::CMP: return "cmp";
    case Opcode::AND: return "and";
    case Opcode::OR: return "or";
    }
    return "unknown";
}

enum class OperandKind {
    Register,
    Immediate,
    Memory,
    Predicate
};

// A teaching-oriented tagged struct.
//
// In production you might compress this with a union or switch to std::variant.
// Here we keep every field visible so readers can easily follow the encoder and
// decoder logic.
struct Operand {
    OperandKind kind = OperandKind::Immediate;
    std::uint32_t reg = 0;
    std::int32_t imm = 0;
    std::uint32_t base_reg = 0;
    std::int32_t offset = 0;
    std::uint32_t pred = 0;

    static Operand makeRegister(std::uint32_t index) {
        Operand operand;
        operand.kind = OperandKind::Register;
        operand.reg = index;
        return operand;
    }

    static Operand makeImmediate(std::int32_t value) {
        Operand operand;
        operand.kind = OperandKind::Immediate;
        operand.imm = value;
        return operand;
    }

    static Operand makeMemory(std::uint32_t base_register, std::int32_t memory_offset) {
        Operand operand;
        operand.kind = OperandKind::Memory;
        operand.base_reg = base_register;
        operand.offset = memory_offset;
        return operand;
    }

    static Operand makePredicate(std::uint32_t index) {
        Operand operand;
        operand.kind = OperandKind::Predicate;
        operand.pred = index;
        return operand;
    }

    bool operator==(const Operand& other) const {
        if (kind != other.kind) {
            return false;
        }
        switch (kind) {
        case OperandKind::Register:
            return reg == other.reg;
        case OperandKind::Immediate:
            return imm == other.imm;
        case OperandKind::Memory:
            return base_reg == other.base_reg && offset == other.offset;
        case OperandKind::Predicate:
            return pred == other.pred;
        }
        return false;
    }
};

// Memory objects are program metadata rather than encoded instruction bits.
// Instructions refer to memory by base register + offset; the object catalog is
// useful to a front-end builder or generator, but the instruction stream itself
// does not need the richer object description.
struct MemoryObject {
    std::string name;
    std::uint32_t base_register = 0;
    std::int32_t base_offset = 0;
    std::uint32_t size = 0;

    bool operator==(const MemoryObject& other) const {
        return name == other.name && base_register == other.base_register &&
               base_offset == other.base_offset && size == other.size;
    }
};

struct Instruction {
    Opcode opcode = Opcode::NOP;
    std::vector<Operand> operands;

    bool operator==(const Instruction& other) const {
        return opcode == other.opcode && operands == other.operands;
    }
};

struct BasicBlock {
    std::string name;
    std::vector<Instruction> instructions;

    bool operator==(const BasicBlock& other) const {
        return name == other.name && instructions == other.instructions;
    }
};

struct Program {
    std::vector<BasicBlock> blocks;
    std::vector<MemoryObject> memory_objects;

    bool operator==(const Program& other) const {
        return blocks == other.blocks && memory_objects == other.memory_objects;
    }
};

inline std::size_t instructionCount(const Program& program) {
    std::size_t count = 0;
    for (const BasicBlock& block : program.blocks) {
        count += block.instructions.size();
    }
    return count;
}

} // namespace cmdgen
