/*
 * decoder.h — Table-driven instruction decoder for ToyGPU ISA.
 *
 * ===== INTERVIEW LESSON =====
 * "How do you decode instructions from a binary?"
 * Answer: Mask-and-match against the opcode table. For each word, iterate
 * the table and find the entry where (word & entry.mask) == entry.match_value.
 * This is O(N) in table size. For large ISAs (hundreds of entries), you'd
 * build a DECISION TREE keyed on the opcode bits to get O(1) lookup — but
 * the linear scan is correct and sufficient for small tables.
 *
 * ROBUSTNESS CONTRACT:
 * - The decoder NEVER crashes, even on random/corrupt input.
 * - The decoder NEVER hangs — it always advances by at least one word.
 * - Unrecognized words produce `.unknown 0xNNNNNNNN`.
 * - A two-word instruction at the very end (no room for word2) is `.unknown`.
 * These properties are critical for tools that process untrusted binaries.
 * =====
 */

#ifndef DECODER_H
#define DECODER_H

#include "toy_isa.h"
#include <vector>
#include <string>

namespace toyisa {

// Decode a single instruction starting at `words[index]`.
// Returns a DecodedInst and advances `index` past the consumed words.
// If decoding fails, emits `.unknown` and advances by 1 word.
DecodedInst decode_one(const std::vector<uint32_t>& words, size_t& index, uint32_t base_address);

// Decode an entire binary (vector of 32-bit words) into a list of instructions.
std::vector<DecodedInst> decode_all(const std::vector<uint32_t>& words, uint32_t base_address = 0);

// Format a decoded instruction as a human-readable string.
// Example: "0x0010:  @!P2 ADDI R5, R6, -1"
std::string format_inst(const DecodedInst& inst);

} // namespace toyisa

#endif // DECODER_H
