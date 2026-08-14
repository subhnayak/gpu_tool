#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "builder.h"
#include "decoder.h"
#include "encoder.h"
#include "random_gen.h"
#include "visitor.h"

namespace cmdgen {
namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<Instruction> flatten(const Program& program) {
    std::vector<Instruction> instructions;
    for (const BasicBlock& block : program.blocks) {
        instructions.insert(instructions.end(), block.instructions.begin(), block.instructions.end());
    }
    return instructions;
}

Program singleInstructionProgram(const Instruction& instruction) {
    Program program;
    BasicBlock block;
    block.name = "entry";
    block.instructions.push_back(instruction);
    program.blocks.push_back(block);
    return program;
}

Instruction makeRandomInstruction(Opcode opcode, std::mt19937_64& rng) {
    std::uniform_int_distribution<std::uint32_t> reg_dist(0U, 15U);
    std::uniform_int_distribution<std::uint32_t> pred_dist(0U, 3U);
    std::uniform_int_distribution<int> imm_dist(-32, 31);

    switch (opcode) {
    case Opcode::NOP:
        return Instruction{Opcode::NOP, {}};
    case Opcode::ADD:
        return Instruction{Opcode::ADD, {Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    case Opcode::MUL:
        return Instruction{Opcode::MUL, {Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    case Opcode::FMA:
        return Instruction{Opcode::FMA, {Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    case Opcode::LOAD:
        return Instruction{Opcode::LOAD, {Operand::makeRegister(reg_dist(rng)), Operand::makeMemory(reg_dist(rng), imm_dist(rng))}};
    case Opcode::STORE:
        return Instruction{Opcode::STORE, {Operand::makeRegister(reg_dist(rng)), Operand::makeMemory(reg_dist(rng), imm_dist(rng))}};
    case Opcode::BRANCH:
        return Instruction{Opcode::BRANCH, {Operand::makePredicate(pred_dist(rng)), Operand::makeImmediate(imm_dist(rng))}};
    case Opcode::CMP:
        return Instruction{Opcode::CMP, {Operand::makePredicate(pred_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    case Opcode::AND:
        return Instruction{Opcode::AND, {Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    case Opcode::OR:
        return Instruction{Opcode::OR, {Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng)), Operand::makeRegister(reg_dist(rng))}};
    }
    return Instruction{};
}

void testRegistry() {
    auto encoder_a = EncoderRegistry::instance().create("A");
    auto encoder_b = EncoderRegistry::instance().create("B");
    expect(static_cast<bool>(encoder_a), "Registry must create encoder A");
    expect(static_cast<bool>(encoder_b), "Registry must create encoder B");
}

void testRoundTripExamples() {
    WorkloadBuilder builder;
    builder.block("entry")
        .nop()
        .add(1, 2, 3)
        .mul(4, 5, 6)
        .fma(7, 1, 2, 3)
        .load(8, 9, -12)
        .store(10, 11, 16)
        .cmp(2, 12, 13)
        .bit_and(14, 1, 2)
        .bit_or(15, 3, 4)
        .branch("entry", 1);
    const Program program = builder.build();
    const std::vector<Instruction> original = flatten(program);

    for (Architecture arch : {Architecture::ARCH_A, Architecture::ARCH_B}) {
        auto encoder = EncoderRegistry::instance().create(architectureName(arch));
        expect(static_cast<bool>(encoder), "Missing encoder for round-trip test");
        Decoder decoder;

        const std::vector<std::uint32_t> words = encoder->encodeProgram(program);
        const Program decoded = decoder.decodeProgram(arch, words);
        const std::vector<Instruction> decoded_instructions = flatten(decoded);

        expect(original.size() == decoded_instructions.size(), "Round-trip instruction count mismatch");
        for (std::size_t i = 0; i < original.size(); ++i) {
            expect(original[i] == decoded_instructions[i], "Round-trip instruction mismatch");
        }

        const std::vector<std::uint32_t> words_again = encoder->encodeProgram(decoded);
        expect(words == words_again, "Re-encoded words must match original words");
    }
}

void testRandomRoundTrip() {
    std::mt19937_64 rng(1234567U);
    for (Architecture arch : {Architecture::ARCH_A, Architecture::ARCH_B}) {
        auto encoder = EncoderRegistry::instance().create(architectureName(arch));
        expect(static_cast<bool>(encoder), "Missing encoder for random round-trip test");
        Decoder decoder;

        for (int i = 0; i < 200; ++i) {
            const auto ops = allOpcodes();
            const Opcode opcode = ops[static_cast<std::size_t>(i) % ops.size()];
            const Instruction instruction = makeRandomInstruction(opcode, rng);
            const Program program = singleInstructionProgram(instruction);
            const std::vector<std::uint32_t> words = encoder->encodeProgram(program);
            const Program decoded = decoder.decodeProgram(arch, words);
            const std::vector<Instruction> decoded_instructions = flatten(decoded);
            expect(decoded_instructions.size() == 1U, "Single instruction decode failed");
            expect(decoded_instructions[0] == instruction, "Random round-trip mismatch");
        }
    }
}

void testSignExtensionEdges() {
    const std::vector<int> edge_values = {-2048, -1, 0, 2047};
    for (Architecture arch : {Architecture::ARCH_A, Architecture::ARCH_B}) {
        auto encoder = EncoderRegistry::instance().create(architectureName(arch));
        expect(static_cast<bool>(encoder), "Missing encoder for sign-extension test");
        Decoder decoder;

        for (int value : edge_values) {
            const std::vector<Instruction> instructions = {
                Instruction{Opcode::BRANCH, {Operand::makePredicate(1), Operand::makeImmediate(value)}},
                Instruction{Opcode::LOAD, {Operand::makeRegister(2), Operand::makeMemory(3, value)}},
                Instruction{Opcode::STORE, {Operand::makeRegister(4), Operand::makeMemory(5, value)}}
            };

            for (const Instruction& instruction : instructions) {
                const Program program = singleInstructionProgram(instruction);
                const std::vector<std::uint32_t> words = encoder->encodeProgram(program);
                const Program decoded = decoder.decodeProgram(arch, words);
                expect(flatten(decoded)[0] == instruction, "Sign-extension edge case failed");
            }
        }
    }
}

void testDeterminism() {
    RandomGeneratorConfig config;
    config.master_seed = 42U;
    config.instruction_count = 64U;
    config.block_count = 4U;
    config.register_count = 8U;

    RandomProgramGenerator generator_a(config);
    RandomProgramGenerator generator_b(config);

    auto encoder = EncoderRegistry::instance().create("A");
    expect(static_cast<bool>(encoder), "Missing encoder A for determinism test");

    const std::vector<std::uint32_t> words_one = encoder->encodeProgram(generator_a.generate());
    const std::vector<std::uint32_t> words_two = encoder->encodeProgram(generator_b.generate());
    expect(words_one == words_two, "Same seed must produce byte-identical output");

    config.master_seed = 43U;
    RandomProgramGenerator generator_c(config);
    const std::vector<std::uint32_t> words_three = encoder->encodeProgram(generator_c.generate());
    expect(words_one != words_three, "Different seeds should produce different output");
}

void testVisitorOutput() {
    WorkloadBuilder builder;
    builder.block("entry").add(1, 2, 3).load(4, 5, -8).branch("exit", 0).block("exit").nop();
    const Program program = builder.build();

    std::ostringstream printed;
    PrinterVisitor printer(printed);
    walkProgram(program, printer);

    const std::string expected_print =
        "program:\n"
        "bb0 (entry):\n"
        "  add r1, r2, r3\n"
        "  load r4, [r5 - 8]\n"
        "  branch p0, bb1\n"
        "bb1 (exit):\n"
        "  nop\n";
    expect(printed.str() == expected_print, "Printer output changed unexpectedly");

    StatsVisitor stats;
    walkProgram(program, stats);
    std::ostringstream report;
    stats.report(report);

    const std::string expected_stats =
        "Instruction counts:\n"
        "  nop: 1\n"
        "  add: 1\n"
        "  mul: 0\n"
        "  fma: 0\n"
        "  load: 1\n"
        "  store: 0\n"
        "  branch: 1\n"
        "  cmp: 0\n"
        "  and: 0\n"
        "  or: 0\n"
        "Register histogram:\n"
        "  r1: 1\n"
        "  r2: 1\n"
        "  r3: 1\n"
        "  r4: 1\n"
        "  r5: 1\n"
        "Predicate operands: 1\n"
        "Immediate operands: 1\n"
        "Memory operands: 1\n";
    expect(report.str() == expected_stats, "Stats output changed unexpectedly");
}

} // namespace
} // namespace cmdgen

int main() {
    using namespace cmdgen;

    try {
        testRegistry();
        testRoundTripExamples();
        testRandomRoundTrip();
        testSignExtensionEdges();
        testDeterminism();
        testVisitorOutput();
        std::cout << "All cmdgen tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failure: " << ex.what() << "\n";
        return 1;
    }
}
