#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ir.h"

namespace cmdgen {

class WorkloadBuilder {
public:
    WorkloadBuilder() = default;

    WorkloadBuilder& block(const std::string& name) {
        auto it = block_lookup_.find(name);
        if (it == block_lookup_.end()) {
            BasicBlock new_block;
            new_block.name = name;
            program_.blocks.push_back(std::move(new_block));
            current_block_index_ = program_.blocks.size() - 1;
            block_lookup_[name] = current_block_index_;
        } else {
            current_block_index_ = it->second;
        }
        return *this;
    }

    WorkloadBuilder& nop() {
        append(Instruction{Opcode::NOP, {}});
        return *this;
    }

    WorkloadBuilder& add(std::uint32_t dst, std::uint32_t src0, std::uint32_t src1) {
        append(Instruction{Opcode::ADD, {Operand::makeRegister(dst), Operand::makeRegister(src0), Operand::makeRegister(src1)}});
        return *this;
    }

    WorkloadBuilder& mul(std::uint32_t dst, std::uint32_t src0, std::uint32_t src1) {
        append(Instruction{Opcode::MUL, {Operand::makeRegister(dst), Operand::makeRegister(src0), Operand::makeRegister(src1)}});
        return *this;
    }

    WorkloadBuilder& fma(std::uint32_t dst, std::uint32_t src0, std::uint32_t src1, std::uint32_t src2) {
        append(Instruction{Opcode::FMA, {Operand::makeRegister(dst), Operand::makeRegister(src0), Operand::makeRegister(src1), Operand::makeRegister(src2)}});
        return *this;
    }

    WorkloadBuilder& load(std::uint32_t dst, const MemoryObject& object) {
        rememberMemory(object);
        return load(dst, object.base_register, object.base_offset);
    }

    WorkloadBuilder& load(std::uint32_t dst, std::uint32_t base_register, std::int32_t offset) {
        append(Instruction{Opcode::LOAD, {Operand::makeRegister(dst), Operand::makeMemory(base_register, offset)}});
        return *this;
    }

    WorkloadBuilder& store(std::uint32_t src, const MemoryObject& object) {
        rememberMemory(object);
        return store(src, object.base_register, object.base_offset);
    }

    WorkloadBuilder& store(std::uint32_t src, std::uint32_t base_register, std::int32_t offset) {
        append(Instruction{Opcode::STORE, {Operand::makeRegister(src), Operand::makeMemory(base_register, offset)}});
        return *this;
    }

    WorkloadBuilder& cmp(std::uint32_t predicate, std::uint32_t src0, std::uint32_t src1) {
        append(Instruction{Opcode::CMP, {Operand::makePredicate(predicate), Operand::makeRegister(src0), Operand::makeRegister(src1)}});
        return *this;
    }

    WorkloadBuilder& branch(const std::string& target_label, std::uint32_t predicate = 0) {
        ensureCurrentBlock();
        Instruction instruction{Opcode::BRANCH, {Operand::makePredicate(predicate), Operand::makeImmediate(0)}};
        program_.blocks[current_block_index_].instructions.push_back(instruction);
        pending_branches_.push_back(PendingBranch{current_block_index_,
                                                  program_.blocks[current_block_index_].instructions.size() - 1,
                                                  target_label});
        return *this;
    }

    WorkloadBuilder& bit_and(std::uint32_t dst, std::uint32_t src0, std::uint32_t src1) {
        append(Instruction{Opcode::AND, {Operand::makeRegister(dst), Operand::makeRegister(src0), Operand::makeRegister(src1)}});
        return *this;
    }

    WorkloadBuilder& bit_or(std::uint32_t dst, std::uint32_t src0, std::uint32_t src1) {
        append(Instruction{Opcode::OR, {Operand::makeRegister(dst), Operand::makeRegister(src0), Operand::makeRegister(src1)}});
        return *this;
    }

    Program build() {
        for (const PendingBranch& branch_fixup : pending_branches_) {
            const auto it = block_lookup_.find(branch_fixup.target_label);
            if (it == block_lookup_.end()) {
                throw std::runtime_error("Unknown branch target label: " + branch_fixup.target_label);
            }
            program_.blocks[branch_fixup.block_index]
                .instructions[branch_fixup.instruction_index]
                .operands[1] = Operand::makeImmediate(static_cast<std::int32_t>(it->second));
        }
        pending_branches_.clear();
        return program_;
    }

private:
    struct PendingBranch {
        std::size_t block_index = 0;
        std::size_t instruction_index = 0;
        std::string target_label;
    };

    void ensureCurrentBlock() {
        if (program_.blocks.empty()) {
            block("entry");
        }
    }

    void append(const Instruction& instruction) {
        ensureCurrentBlock();
        program_.blocks[current_block_index_].instructions.push_back(instruction);
    }

    void rememberMemory(const MemoryObject& object) {
        for (const MemoryObject& existing : program_.memory_objects) {
            if (existing.name == object.name) {
                return;
            }
        }
        program_.memory_objects.push_back(object);
    }

    Program program_;
    std::size_t current_block_index_ = 0;
    std::map<std::string, std::size_t> block_lookup_;
    std::vector<PendingBranch> pending_branches_;
};

} // namespace cmdgen
