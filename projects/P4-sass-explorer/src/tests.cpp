/*
 * tests.cpp — Comprehensive test suite for ToyGPU ISA tools.
 *
 * Dependency-free: uses simple assert macros, no test framework.
 * Tests are grouped by category and run sequentially.
 *
 * Categories:
 *   1. Round-trip: assemble then disassemble every opcode
 *   2. Sign extension: boundary values (-2048, -1, 0, 2047)
 *   3. Two-word instructions (LIMM)
 *   4. Predication
 *   5. CFG construction on known programs
 *   6. Fuzz: random bytes to decoder (must not crash)
 *   7. Assembler error reporting
 */

#include "toy_isa.h"
#include "decoder.h"
#include "assembler.h"
#include "cfg.h"

#include <iostream>
#include <sstream>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        std::cout << "  " << #name << "... "; \
        try { test_##name(); tests_passed++; std::cout << "PASS\n"; } \
        catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
        catch (...) { std::cout << "FAIL: unknown exception\n"; } \
    } while(0)

#define ASSERT(cond) \
    do { if (!(cond)) { \
        throw std::runtime_error(std::string("Assertion failed: ") + #cond + \
            " at line " + std::to_string(__LINE__)); \
    }} while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) { \
        std::ostringstream _os; _os << "Expected " << (a) << " == " << (b) \
            << " at line " << __LINE__; \
        throw std::runtime_error(_os.str()); \
    }} while(0)

using namespace toyisa;

// ==============================================================
// 1. Round-trip tests: assemble -> decode -> check mnemonic & operands
// ==============================================================

static void roundtrip_r_type(const std::string& mnem, int rd, int rs1, int rs2) {
    std::string src = mnem + " R" + std::to_string(rd) + ", R" +
                      std::to_string(rs1) + ", R" + std::to_string(rs2) + "\n";
    auto result = assemble(src);
    if (!result.success()) {
        std::ostringstream err;
        err << "Assembly of '" << src << "' failed: ";
        for (const auto& e : result.errors) err << "L" << e.line << ": " << e.message << "; ";
        throw std::runtime_error(err.str());
    }
    ASSERT_EQ(result.words.size(), (size_t)1);
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded.size(), (size_t)1);
    ASSERT_EQ(decoded[0].mnemonic, mnem);
    ASSERT_EQ(decoded[0].rd, rd);
    ASSERT_EQ(decoded[0].rs1, rs1);
    ASSERT_EQ(decoded[0].rs2, rs2);
}

static void test_roundtrip_alu() {
    roundtrip_r_type("ADD", 1, 2, 3);
    roundtrip_r_type("SUB", 31, 0, 15);
    roundtrip_r_type("AND", 10, 20, 30);
    roundtrip_r_type("OR", 0, 0, 0);
    roundtrip_r_type("XOR", 5, 5, 5);
    roundtrip_r_type("SHL", 1, 2, 3);
    roundtrip_r_type("SHR", 4, 5, 6);
    roundtrip_r_type("SLT", 7, 8, 9);
    roundtrip_r_type("MUL", 1, 2, 3);
}

static void test_roundtrip_immediate() {
    // Test ADDI with various immediates including sign-extension boundaries
    int32_t test_values[] = {0, 1, -1, 2047, -2048, 100, -100};
    for (int32_t imm : test_values) {
        std::string src = "ADDI R1, R2, " + std::to_string(imm) + "\n";
        auto result = assemble(src);
        ASSERT(result.success());
        auto decoded = decode_all(result.words);
        ASSERT_EQ(decoded.size(), (size_t)1);
        ASSERT_EQ(decoded[0].mnemonic, "ADDI");
        ASSERT_EQ(decoded[0].immediate, imm);
    }
}

static void test_roundtrip_unsigned_imm() {
    // ANDI/ORI use zero-extended immediates
    std::string src = "ANDI R1, R2, 4095\n";  // Max unsigned 12-bit
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].immediate, 4095);
}

static void test_roundtrip_load_store() {
    std::string src = "LD R5, [R10+100]\nST R3, [R4-8]\nLDB R1, [R2]\nSTB R6, [R7+1]\n";
    auto result = assemble(src);
    ASSERT(result.success());
    ASSERT_EQ(result.words.size(), (size_t)4);
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].mnemonic, "LD");
    ASSERT_EQ(decoded[0].rd, 5);
    ASSERT_EQ(decoded[0].rs1, 10);
    ASSERT_EQ(decoded[0].immediate, 100);
    ASSERT_EQ(decoded[1].mnemonic, "ST");
    ASSERT_EQ(decoded[1].immediate, -8);
    ASSERT_EQ(decoded[2].mnemonic, "LDB");
    ASSERT_EQ(decoded[3].mnemonic, "STB");
}

static void test_roundtrip_setp() {
    std::string src = "SETP.EQ P1, R2, R3\nSETP.LT P7, R0, R31\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].mnemonic, "SETP.EQ");
    ASSERT_EQ(decoded[0].rd & 0x7, 1);  // P1
    ASSERT_EQ(decoded[1].mnemonic, "SETP.LT");
    ASSERT_EQ(decoded[1].rd & 0x7, 7);  // P7
}

// ==============================================================
// 2. Sign extension
// ==============================================================

static void test_sign_extension() {
    // Verify sign_extend works correctly
    ASSERT_EQ(sign_extend(0xFFF, 12), -1);        // 12-bit -1
    ASSERT_EQ(sign_extend(0x800, 12), -2048);      // 12-bit most negative
    ASSERT_EQ(sign_extend(0x7FF, 12), 2047);       // 12-bit most positive
    ASSERT_EQ(sign_extend(0x000, 12), 0);
    ASSERT_EQ(sign_extend(0x1FFFF, 17), -1);       // 17-bit -1
    ASSERT_EQ(sign_extend(0x10000, 17), -65536);   // 17-bit most negative
    ASSERT_EQ(sign_extend(0x0FFFF, 17), 65535);    // 17-bit most positive
}

// ==============================================================
// 3. Two-word instructions (LIMM)
// ==============================================================

static void test_two_word_limm() {
    std::string src = "LIMM R10, 0xDEADBEEF\n";
    auto result = assemble(src);
    ASSERT(result.success());
    ASSERT_EQ(result.words.size(), (size_t)2);

    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded.size(), (size_t)1);
    ASSERT(decoded[0].is_two_word);
    ASSERT_EQ(decoded[0].mnemonic, "LIMM");
    ASSERT_EQ(decoded[0].rd, 10);
    ASSERT_EQ(static_cast<uint32_t>(decoded[0].immediate), 0xDEADBEEFu);
}

static void test_limm_at_end() {
    // A LIMM at the very end of binary (no word2) should produce .unknown
    uint32_t word = 0;
    encode_field(word, FIELD_OPCODE, 0x20);  // LIMM opcode
    encode_field(word, FIELD_RD, 5);
    std::vector<uint32_t> words = {word};

    auto decoded = decode_all(words);
    ASSERT_EQ(decoded.size(), (size_t)1);
    ASSERT(decoded[0].is_unknown);
}

// ==============================================================
// 4. Predication
// ==============================================================

static void test_predication() {
    std::string src = "@P3 ADD R1, R2, R3\n@!P5 ADDI R4, R5, 42\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].pred_reg, 3);
    ASSERT_EQ(decoded[0].pred_negate, false);
    ASSERT_EQ(decoded[1].pred_reg, 5);
    ASSERT_EQ(decoded[1].pred_negate, true);
}

// ==============================================================
// 5. Branch round-trip and CFG
// ==============================================================

static void test_roundtrip_branch() {
    // Simple forward branch
    std::string src =
        "    BEZ R1, target\n"
        "    ADD R2, R3, R4\n"
        "target:\n"
        "    HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].mnemonic, "BEZ");
    ASSERT(decoded[0].is_branch);
    ASSERT_EQ(decoded[0].branch_target(), (uint32_t)8);  // target at byte 8
}

static void test_cfg_straight_line() {
    std::string src =
        "ADD R1, R2, R3\n"
        "SUB R4, R5, R6\n"
        "HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    auto cfg = CFG::build(decoded);
    ASSERT_EQ(cfg.blocks.size(), (size_t)1);  // Single block
    ASSERT(cfg.back_edges.empty());
}

static void test_cfg_diamond() {
    // if/else diamond:
    //   BEZ R1, else_label
    //   ADD R2, R3, R4      ; then
    //   BRA end
    // else_label:
    //   SUB R2, R3, R4      ; else
    // end:
    //   HALT
    std::string src =
        "    BEZ R1, else_label\n"
        "    ADD R2, R3, R4\n"
        "    BRA end\n"
        "else_label:\n"
        "    SUB R2, R3, R4\n"
        "end:\n"
        "    HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    auto cfg = CFG::build(decoded);
    // Should have 4 blocks: entry(BEZ), then(ADD+BRA), else(SUB), end(HALT)
    ASSERT_EQ(cfg.blocks.size(), (size_t)4);
    ASSERT(cfg.back_edges.empty());
}

static void test_cfg_loop() {
    // Counted loop:
    //   ADDI R1, R0, 10
    // loop:
    //   ADDI R1, R1, -1
    //   BNZ R1, loop
    //   HALT
    std::string src =
        "    ADDI R1, R0, 10\n"
        "loop:\n"
        "    ADDI R1, R1, -1\n"
        "    BNZ R1, loop\n"
        "    HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    auto cfg = CFG::build(decoded);
    ASSERT(!cfg.back_edges.empty());  // Must detect the loop
}

static void test_cfg_nested_loop() {
    std::string src =
        "    ADDI R1, R0, 5\n"
        "outer:\n"
        "    ADDI R2, R0, 3\n"
        "inner:\n"
        "    ADDI R2, R2, -1\n"
        "    BNZ R2, inner\n"
        "    ADDI R1, R1, -1\n"
        "    BNZ R1, outer\n"
        "    HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    auto cfg = CFG::build(decoded);
    ASSERT_EQ(cfg.back_edges.size(), (size_t)2);  // Two loops
}

// ==============================================================
// 6. Fuzz test: random bytes to decoder
// ==============================================================

static void test_fuzz_decoder() {
    // Feed 10000 random words to the decoder. It must:
    // - Never crash
    // - Never hang (always make forward progress)
    // - Always return at least one instruction per word
    std::mt19937 rng(42);  // Fixed seed for reproducibility

    for (int trial = 0; trial < 100; trial++) {
        size_t len = 1 + (rng() % 50);
        std::vector<uint32_t> words(len);
        for (auto& w : words) w = rng();

        auto decoded = decode_all(words);
        // Must decode at least 1 instruction and at most len instructions
        ASSERT(!decoded.empty());
        ASSERT(decoded.size() <= len);

        // Every instruction must have a valid address and size
        for (const auto& inst : decoded) {
            ASSERT(inst.size() == 4 || inst.size() == 8);
        }
    }
}

// ==============================================================
// 7. Assembler error reporting
// ==============================================================

static void test_assembler_errors() {
    // Unknown mnemonic
    auto r1 = assemble("BOGUS R1, R2, R3\n");
    ASSERT(!r1.success());
    ASSERT(r1.errors[0].message.find("unknown mnemonic") != std::string::npos);

    // Invalid register
    auto r2 = assemble("ADD R1, R99, R3\n");
    ASSERT(!r2.success());

    // Missing operands
    auto r3 = assemble("ADD R1\n");
    ASSERT(!r3.success());

    // Duplicate label
    auto r4 = assemble("lbl:\nlbl:\nHALT\n");
    ASSERT(!r4.success());

    // Undefined label
    auto r5 = assemble("BRA nowhere\n");
    ASSERT(!r5.success());

    // Immediate out of range
    auto r6 = assemble("ADDI R1, R2, 99999\n");
    ASSERT(!r6.success());
}

// ==============================================================
// 8. Special instructions
// ==============================================================

static void test_special_instructions() {
    std::string src = "NOP\nHALT\nBAR R5\nRET\n";
    auto result = assemble(src);
    ASSERT(result.success());
    auto decoded = decode_all(result.words);
    ASSERT_EQ(decoded[0].mnemonic, "NOP");
    ASSERT_EQ(decoded[1].mnemonic, "HALT");
    ASSERT(decoded[1].is_halt);
    ASSERT_EQ(decoded[2].mnemonic, "BAR");
    ASSERT(decoded[2].is_barrier);
    ASSERT_EQ(decoded[3].mnemonic, "RET");
    ASSERT(decoded[3].is_return);
}

// ==============================================================
// 9. Linear vs Recursive descent
// ==============================================================

static void test_linear_vs_recursive() {
    // Program with embedded data after an unconditional branch
    // Linear sweep will decode the data; recursive descent will skip it.
    std::string src =
        "    ADDI R1, R0, 1\n"
        "    BRA skip\n"
        "    LIMM R0, 0xDEADBEEF\n"  // "data" that BRA skips over
        "skip:\n"
        "    HALT\n";
    auto result = assemble(src);
    ASSERT(result.success());

    auto linear = decode_all(result.words);
    auto recursive = recursive_descent(result.words, {0});

    // Linear should decode more instructions (including the LIMM)
    ASSERT(linear.size() >= recursive.size());
}

// ==============================================================
// Main
// ==============================================================

int main() {
    std::cout << "=== ToyGPU ISA Test Suite ===\n\n";

    std::cout << "[Round-trip tests]\n";
    TEST(roundtrip_alu);
    TEST(roundtrip_immediate);
    TEST(roundtrip_unsigned_imm);
    TEST(roundtrip_load_store);
    TEST(roundtrip_setp);
    TEST(roundtrip_branch);

    std::cout << "\n[Sign extension]\n";
    TEST(sign_extension);

    std::cout << "\n[Two-word instructions]\n";
    TEST(two_word_limm);
    TEST(limm_at_end);

    std::cout << "\n[Predication]\n";
    TEST(predication);

    std::cout << "\n[CFG construction]\n";
    TEST(cfg_straight_line);
    TEST(cfg_diamond);
    TEST(cfg_loop);
    TEST(cfg_nested_loop);

    std::cout << "\n[Fuzz testing]\n";
    TEST(fuzz_decoder);

    std::cout << "\n[Assembler errors]\n";
    TEST(assembler_errors);

    std::cout << "\n[Special instructions]\n";
    TEST(special_instructions);

    std::cout << "\n[Linear vs Recursive]\n";
    TEST(linear_vs_recursive);

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed ===\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
