#include "random_gen.h"

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>

#include "builder.h"

namespace cmdgen {
namespace {

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::uint32_t boundedRegister(std::mt19937_64& rng, std::uint32_t register_count) {
    std::uniform_int_distribution<std::uint32_t> dist(0U, register_count - 1U);
    return dist(rng);
}

std::int32_t smallSignedImmediate(std::mt19937_64& rng) {
    static const std::array<std::int32_t, 9> pool = {-16, -12, -8, -4, 0, 4, 8, 12, 16};
    std::uniform_int_distribution<std::size_t> dist(0U, pool.size() - 1U);
    return pool[dist(rng)];
}

std::uint32_t boundedPredicate(std::mt19937_64& rng) {
    std::uniform_int_distribution<std::uint32_t> dist(0U, 3U);
    return dist(rng);
}

Opcode pickWeightedOpcode(std::mt19937_64& rng) {
    // The ordering is stored in a vector, not a hash table, so iteration order is
    // deterministic. Avoiding unordered containers is a simple but important rule
    // for reproducible generators.
    static const std::vector<Opcode> opcodes = {
        Opcode::ADD, Opcode::MUL, Opcode::FMA, Opcode::LOAD, Opcode::STORE,
        Opcode::BRANCH, Opcode::CMP, Opcode::AND, Opcode::OR, Opcode::NOP};
    static const std::vector<int> weights = {20, 15, 10, 12, 12, 8, 10, 6, 6, 1};
    std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
    return opcodes[dist(rng)];
}

} // namespace

std::uint64_t deriveSeed(std::uint64_t master_seed, std::uint64_t component_id) {
    return splitmix64(master_seed ^ splitmix64(component_id + 0xABCDEF1234567890ULL));
}

Program RandomProgramGenerator::generate() const {
    const std::size_t block_count = std::max<std::size_t>(1U, config_.block_count);
    const std::size_t instruction_count_total = config_.instruction_count;
    const std::uint32_t register_count = std::max<std::uint32_t>(1U, config_.register_count);

    // Determinism rules:
    // - seed everything from the user-provided master seed,
    // - never consult wall clock time,
    // - never use pointer values as entropy,
    // - never rely on unordered_map iteration order.
    std::mt19937_64 opcode_rng(deriveSeed(config_.master_seed, 1U));
    std::mt19937_64 operand_rng(deriveSeed(config_.master_seed, 2U));
    std::mt19937_64 branch_rng(deriveSeed(config_.master_seed, 3U));

    std::vector<std::string> block_names;
    block_names.reserve(block_count);
    for (std::size_t i = 0; i < block_count; ++i) {
        block_names.push_back("bb" + std::to_string(i));
    }

    std::vector<MemoryObject> memory_pool = {
        {"weights", 0U, 0, 64U},
        {"activations", 1U % register_count, 16, 128U},
        {"scratch", 2U % register_count, -16, 32U}
    };

    std::vector<Opcode> schedule;
    schedule.reserve(instruction_count_total);
    const auto guaranteed = allOpcodes();
    for (std::size_t i = 0; i < instruction_count_total && i < guaranteed.size(); ++i) {
        schedule.push_back(guaranteed[i]);
    }
    while (schedule.size() < instruction_count_total) {
        schedule.push_back(pickWeightedOpcode(opcode_rng));
    }

    WorkloadBuilder builder;
    for (std::size_t index = 0; index < instruction_count_total; ++index) {
        const std::size_t block_index = std::min(block_count - 1U, (index * block_count) / std::max<std::size_t>(1U, instruction_count_total));
        builder.block(block_names[block_index]);

        const Opcode opcode = schedule[index];
        switch (opcode) {
        case Opcode::NOP:
            builder.nop();
            break;
        case Opcode::ADD:
            builder.add(boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count));
            break;
        case Opcode::MUL:
            builder.mul(boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count));
            break;
        case Opcode::FMA:
            builder.fma(boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count));
            break;
        case Opcode::LOAD:
            builder.load(boundedRegister(operand_rng, register_count),
                         memory_pool[static_cast<std::size_t>(boundedRegister(operand_rng, static_cast<std::uint32_t>(memory_pool.size())))]);
            break;
        case Opcode::STORE:
            builder.store(boundedRegister(operand_rng, register_count),
                          boundedRegister(operand_rng, register_count),
                          smallSignedImmediate(operand_rng));
            break;
        case Opcode::BRANCH:
            builder.branch(block_names[boundedRegister(branch_rng, static_cast<std::uint32_t>(block_names.size()))],
                           boundedPredicate(operand_rng));
            break;
        case Opcode::CMP:
            builder.cmp(boundedPredicate(operand_rng),
                        boundedRegister(operand_rng, register_count),
                        boundedRegister(operand_rng, register_count));
            break;
        case Opcode::AND:
            builder.bit_and(boundedRegister(operand_rng, register_count),
                            boundedRegister(operand_rng, register_count),
                            boundedRegister(operand_rng, register_count));
            break;
        case Opcode::OR:
            builder.bit_or(boundedRegister(operand_rng, register_count),
                           boundedRegister(operand_rng, register_count),
                           boundedRegister(operand_rng, register_count));
            break;
        }
    }

    return builder.build();
}

} // namespace cmdgen
