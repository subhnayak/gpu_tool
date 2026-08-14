#pragma once
// ============================================================================
// random_gen.h — Deterministic constrained-random program generator
// ============================================================================
//
// DETERMINISM RULES (critical for GPU tool development — interview topic):
// -----------------------------------------------------------------------
// A verification/test-generation tool MUST produce identical output when given
// the same seed.  Any violation makes bug reproduction impossible.  The rules:
//
//   1. ALL randomness derives from a single user-supplied master_seed.
//      Sub-components get independent streams via deriveSeed(master, id),
//      which hashes (master_seed, component_id) into a new seed.  This
//      allows adding components without changing existing streams.
//
//   2. NEVER use wall-clock time, process IDs, or pointer values as entropy.
//      These change across runs and destroy reproducibility.
//
//   3. NEVER iterate over std::unordered_map / std::unordered_set and use
//      the iteration order to make decisions.  Hash-table order is
//      implementation-defined and can vary across compilers, stdlib versions,
//      or even runs (ASLR-seeded hashing on some platforms).
//      Use std::map, std::vector, or sorted containers instead.
//
//   4. NEVER use std::rand() or any global PRNG state.  Use an explicit
//      engine instance (std::mt19937_64) whose lifetime you control.
//
//   5. Floating-point intermediates are acceptable for weight selection
//      (std::discrete_distribution) because IEEE-754 arithmetic is
//      deterministic for a given platform.  Cross-platform bitwise
//      identity is NOT guaranteed — if needed, use integer arithmetic only.
//
// These rules mirror real constraints in NVIDIA's test generators.
// ============================================================================

#include <cstddef>
#include <cstdint>

#include "ir.h"

namespace cmdgen {

// ---------------------------------------------------------------------------
// Configuration — everything the generator needs, nothing it shouldn't have.
// No wall-clock, no environment queries.
// ---------------------------------------------------------------------------
struct RandomGeneratorConfig {
    std::uint64_t master_seed = 1;       // Single source of all randomness
    std::size_t instruction_count = 32;  // Total instructions to generate
    std::size_t block_count = 3;         // Number of basic blocks
    std::uint32_t register_count = 8;    // Architectural register file size
};

// Derive a sub-seed from (master_seed, component_id) using splitmix64.
// This lets different generator components (opcode picker, operand picker,
// branch target picker) have independent PRNG streams, so adding a new
// component doesn't perturb the existing ones.
std::uint64_t deriveSeed(std::uint64_t master_seed, std::uint64_t component_id);

// ---------------------------------------------------------------------------
// RandomProgramGenerator
// ---------------------------------------------------------------------------
// Generates a complete Program suitable for encode/decode testing.
// Guarantees: every opcode appears at least once (if instruction_count >=
// number of opcodes), weighted random selection for the remainder, and all
// operands are within legal bounds.
// ---------------------------------------------------------------------------
class RandomProgramGenerator {
public:
    explicit RandomProgramGenerator(RandomGeneratorConfig config) : config_(config) {}

    // generate() is const — it does not mutate the generator, it creates a
    // fresh PRNG from config_.master_seed each time, so calling it twice
    // with the same config produces identical programs.  This is intentional.
    Program generate() const;

private:
    RandomGeneratorConfig config_;
};

} // namespace cmdgen
