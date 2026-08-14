#pragma once

#include <map>
#include <ostream>
#include <sstream>
#include <string>

#include "ir.h"

namespace cmdgen {

class IRVisitor {
public:
    virtual ~IRVisitor() = default;

    virtual void beginProgram(const Program&) {}
    virtual void endProgram(const Program&) {}
    virtual void beginBasicBlock(const BasicBlock&, std::size_t) {}
    virtual void endBasicBlock(const BasicBlock&, std::size_t) {}
    virtual void visitInstruction(const Instruction&, std::size_t, std::size_t) = 0;
};

inline void walkProgram(const Program& program, IRVisitor& visitor) {
    visitor.beginProgram(program);
    for (std::size_t block_index = 0; block_index < program.blocks.size(); ++block_index) {
        const BasicBlock& block = program.blocks[block_index];
        visitor.beginBasicBlock(block, block_index);
        for (std::size_t instruction_index = 0; instruction_index < block.instructions.size(); ++instruction_index) {
            visitor.visitInstruction(block.instructions[instruction_index], block_index, instruction_index);
        }
        visitor.endBasicBlock(block, block_index);
    }
    visitor.endProgram(program);
}

inline std::string operandToString(const Operand& operand) {
    std::ostringstream out;
    switch (operand.kind) {
    case OperandKind::Register:
        out << "r" << operand.reg;
        break;
    case OperandKind::Immediate:
        out << operand.imm;
        break;
    case OperandKind::Memory:
        out << "[r" << operand.base_reg;
        if (operand.offset >= 0) {
            out << " + " << operand.offset;
        } else {
            out << " - " << (-operand.offset);
        }
        out << "]";
        break;
    case OperandKind::Predicate:
        out << "p" << operand.pred;
        break;
    }
    return out.str();
}

class PrinterVisitor : public IRVisitor {
public:
    explicit PrinterVisitor(std::ostream& out) : out_(out) {}

    void beginProgram(const Program&) override {
        out_ << "program:\n";
    }

    void beginBasicBlock(const BasicBlock& block, std::size_t block_index) override {
        out_ << "bb" << block_index << " (" << block.name << "):\n";
    }

    void visitInstruction(const Instruction& instruction,
                          std::size_t,
                          std::size_t) override {
        out_ << "  " << opcodeName(instruction.opcode);
        if (!instruction.operands.empty()) {
            out_ << ' ';
        }
        for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
            if (instruction.opcode == Opcode::BRANCH && i == 1 &&
                instruction.operands[i].kind == OperandKind::Immediate) {
                out_ << "bb" << instruction.operands[i].imm;
            } else {
                out_ << operandToString(instruction.operands[i]);
            }
            if (i + 1 < instruction.operands.size()) {
                out_ << ", ";
            }
        }
        out_ << "\n";
    }

private:
    std::ostream& out_;
};

class StatsVisitor : public IRVisitor {
public:
    void visitInstruction(const Instruction& instruction,
                          std::size_t,
                          std::size_t) override {
        ++instruction_counts_[instruction.opcode];
        for (const Operand& operand : instruction.operands) {
            switch (operand.kind) {
            case OperandKind::Register:
                ++register_histogram_[operand.reg];
                break;
            case OperandKind::Immediate:
                ++immediate_operands_;
                break;
            case OperandKind::Memory:
                ++memory_operands_;
                ++register_histogram_[operand.base_reg];
                break;
            case OperandKind::Predicate:
                ++predicate_operands_;
                break;
            }
        }
    }

    std::size_t countFor(Opcode opcode) const {
        const auto it = instruction_counts_.find(opcode);
        return it == instruction_counts_.end() ? 0U : it->second;
    }

    const std::map<std::uint32_t, std::size_t>& registerHistogram() const {
        return register_histogram_;
    }

    void report(std::ostream& out) const {
        out << "Instruction counts:\n";
        for (Opcode opcode : allOpcodes()) {
            out << "  " << opcodeName(opcode) << ": " << countFor(opcode) << "\n";
        }
        out << "Register histogram:\n";
        for (const auto& entry : register_histogram_) {
            out << "  r" << entry.first << ": " << entry.second << "\n";
        }
        out << "Predicate operands: " << predicate_operands_ << "\n";
        out << "Immediate operands: " << immediate_operands_ << "\n";
        out << "Memory operands: " << memory_operands_ << "\n";
    }

private:
    std::map<Opcode, std::size_t> instruction_counts_;
    std::map<std::uint32_t, std::size_t> register_histogram_;
    std::size_t predicate_operands_ = 0;
    std::size_t immediate_operands_ = 0;
    std::size_t memory_operands_ = 0;
};

// Modern alternative:
//
// A different design could model instructions as std::variant nodes and use
// std::visit for traversal. That often feels lighter when the set of visitors is
// small. The classic Visitor pattern is still useful when adding *new passes*
// frequently matters more than adding *new node types*. That trade-off is a
// concrete example of the expression problem.

} // namespace cmdgen
