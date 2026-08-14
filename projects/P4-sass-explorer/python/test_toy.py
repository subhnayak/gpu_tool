"""
test_toy.py — Tests for the Python ToyGPU assembler/disassembler.

Run: pytest test_toy.py -v

Includes:
  - Round-trip tests for every opcode
  - Randomized property-based round-trip testing
  - Differential testing vs C++ implementation (if binaries available)
"""

import sys
import os
import struct
import random
import subprocess

# Add parent dir to path so we can import our modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from isa_table import OPCODE_TABLE, FMT_R, FMT_I, FMT_B, FMT_L, sign_extend
from toy_asm import assemble
from toy_dis import decode_all, format_inst


def words_from_asm(source):
    """Assemble source, assert success, return words."""
    words, errors, labels = assemble(source)
    assert not errors, f"Assembly errors: {errors}"
    return words


def roundtrip(source):
    """Assemble source, decode result, return decoded instructions."""
    words = words_from_asm(source)
    return decode_all(words)


# ---- Round-trip tests for each format ----

class TestRoundtripALU:
    def test_add(self):
        insts = roundtrip("ADD R1, R2, R3\n")
        assert insts[0].mnemonic == "ADD"
        assert insts[0].rd == 1 and insts[0].rs1 == 2 and insts[0].rs2 == 3

    def test_sub(self):
        insts = roundtrip("SUB R31, R0, R15\n")
        assert insts[0].mnemonic == "SUB"

    def test_all_alu_ops(self):
        for mnem in ["ADD", "SUB", "AND", "OR", "XOR", "SHL", "SHR", "SLT", "MUL"]:
            insts = roundtrip(f"{mnem} R5, R10, R15\n")
            assert insts[0].mnemonic == mnem


class TestRoundtripImmediate:
    def test_addi_positive(self):
        insts = roundtrip("ADDI R1, R2, 100\n")
        assert insts[0].immediate == 100

    def test_addi_negative(self):
        insts = roundtrip("ADDI R1, R2, -1\n")
        assert insts[0].immediate == -1

    def test_addi_boundary_values(self):
        for val in [0, 1, -1, 2047, -2048]:
            insts = roundtrip(f"ADDI R1, R0, {val}\n")
            assert insts[0].immediate == val, f"Failed for {val}: got {insts[0].immediate}"

    def test_unsigned_imm(self):
        insts = roundtrip("ANDI R1, R2, 4095\n")
        assert insts[0].immediate == 4095


class TestRoundtripLoadStore:
    def test_load(self):
        insts = roundtrip("LD R5, [R10+100]\n")
        assert insts[0].mnemonic == "LD"
        assert insts[0].rd == 5 and insts[0].rs1 == 10 and insts[0].immediate == 100

    def test_store_negative_offset(self):
        insts = roundtrip("ST R3, [R4-8]\n")
        assert insts[0].immediate == -8


class TestRoundtripBranch:
    def test_forward_branch(self):
        insts = roundtrip("BEZ R1, target\nADD R2, R3, R4\ntarget:\nHALT\n")
        assert insts[0].is_branch
        assert insts[0].branch_target() == 8  # target at byte 8


class TestRoundtripTwoWord:
    def test_limm(self):
        insts = roundtrip("LIMM R10, 0xDEADBEEF\n")
        assert len(insts) == 1
        assert insts[0].is_two_word
        assert (insts[0].immediate & 0xFFFFFFFF) == 0xDEADBEEF


class TestPredication:
    def test_pred(self):
        insts = roundtrip("@P3 ADD R1, R2, R3\n")
        assert insts[0].pred_reg == 3
        assert not insts[0].pred_negate

    def test_pred_negate(self):
        insts = roundtrip("@!P5 ADDI R4, R5, 42\n")
        assert insts[0].pred_reg == 5
        assert insts[0].pred_negate


class TestSpecial:
    def test_nop_halt_ret(self):
        insts = roundtrip("NOP\nHALT\nRET\n")
        assert insts[0].mnemonic == "NOP"
        assert insts[1].mnemonic == "HALT" and insts[1].is_halt
        assert insts[2].mnemonic == "RET" and insts[2].is_return


class TestSignExtension:
    def test_sign_extend_12bit(self):
        assert sign_extend(0xFFF, 12) == -1
        assert sign_extend(0x800, 12) == -2048
        assert sign_extend(0x7FF, 12) == 2047
        assert sign_extend(0x000, 12) == 0

    def test_sign_extend_17bit(self):
        assert sign_extend(0x1FFFF, 17) == -1
        assert sign_extend(0x10000, 17) == -65536


class TestFuzz:
    """Feed random words to the decoder — it must never crash."""
    def test_random_words(self):
        rng = random.Random(42)
        for _ in range(100):
            length = rng.randint(1, 50)
            words = [rng.randint(0, 0xFFFFFFFF) for _ in range(length)]
            decoded = decode_all(words)
            assert len(decoded) >= 1
            assert len(decoded) <= length


class TestRandomizedRoundtrip:
    """Property-based testing: assemble random valid instructions, decode, verify."""
    def test_random_r_type(self):
        rng = random.Random(123)
        r_mnems = ["ADD", "SUB", "AND", "OR", "XOR", "SHL", "SHR", "SLT", "MUL"]
        for _ in range(200):
            m = rng.choice(r_mnems)
            rd, rs1, rs2 = rng.randint(0, 31), rng.randint(0, 31), rng.randint(0, 31)
            src = f"{m} R{rd}, R{rs1}, R{rs2}\n"
            insts = roundtrip(src)
            assert insts[0].mnemonic == m
            assert insts[0].rd == rd
            assert insts[0].rs1 == rs1
            assert insts[0].rs2 == rs2

    def test_random_immediate(self):
        rng = random.Random(456)
        for _ in range(200):
            rd, rs1 = rng.randint(0, 31), rng.randint(0, 31)
            imm = rng.randint(-2048, 2047)
            src = f"ADDI R{rd}, R{rs1}, {imm}\n"
            insts = roundtrip(src)
            assert insts[0].immediate == imm


class TestAssemblerErrors:
    def test_unknown_mnemonic(self):
        _, errors, _ = assemble("BOGUS R1, R2, R3\n")
        assert errors

    def test_duplicate_label(self):
        _, errors, _ = assemble("lbl:\nlbl:\nHALT\n")
        assert errors

    def test_undefined_label(self):
        _, errors, _ = assemble("BRA nowhere\n")
        assert errors


class TestDifferential:
    """
    Differential testing: compare Python and C++ assembler output.

    THIS IS EXACTLY HOW REAL DISASSEMBLERS ARE VALIDATED.
    You build two independent implementations and feed them the same input.
    Any disagreement is a bug in one (or both). This catches:
      - Sign extension errors
      - Field encoding offset mistakes
      - Off-by-one in branch offset computation
      - Endianness bugs

    This test only runs if the C++ toyasm binary is available.
    """

    def _find_cpp_binary(self):
        """Look for the C++ toyasm in common build locations."""
        base = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(base, '..', 'build', 'Release', 'toyasm.exe'),
            os.path.join(base, '..', 'build', 'Debug', 'toyasm.exe'),
            os.path.join(base, '..', 'build', 'toyasm.exe'),
            os.path.join(base, '..', 'build', 'toyasm'),
        ]
        for c in candidates:
            if os.path.isfile(c):
                return os.path.abspath(c)
        return None

    def test_differential_if_available(self):
        cpp_asm = self._find_cpp_binary()
        if cpp_asm is None:
            import pytest
            pytest.skip("C++ toyasm binary not found — build with CMake first")

        test_programs = [
            "ADD R1, R2, R3\nSUB R4, R5, R6\nHALT\n",
            "ADDI R1, R0, -1\nADDI R2, R0, 2047\nADDI R3, R0, -2048\nHALT\n",
            "LIMM R10, 0xDEADBEEF\nHALT\n",
            "@P3 ADD R1, R2, R3\n@!P5 ADDI R4, R5, 42\nHALT\n",
        ]

        for prog in test_programs:
            # Python assembly
            py_words, py_errors, _ = assemble(prog)
            assert not py_errors

            # C++ assembly: write source to temp, invoke, read binary
            src_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_diff_test.asm')
            bin_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_diff_test.bin')
            try:
                with open(src_path, 'w') as f:
                    f.write(prog)
                result = subprocess.run([cpp_asm, src_path, bin_path],
                                       capture_output=True, text=True, timeout=10)
                assert result.returncode == 0, f"C++ assembler failed: {result.stderr}"

                with open(bin_path, 'rb') as f:
                    data = f.read()
                cpp_words = [struct.unpack('<I', data[i:i+4])[0]
                             for i in range(0, len(data), 4)]

                assert py_words == cpp_words, (
                    f"Differential mismatch!\n"
                    f"  Program: {prog!r}\n"
                    f"  Python:  {[f'0x{w:08x}' for w in py_words]}\n"
                    f"  C++:     {[f'0x{w:08x}' for w in cpp_words]}"
                )
            finally:
                for p in (src_path, bin_path):
                    if os.path.exists(p):
                        os.remove(p)
