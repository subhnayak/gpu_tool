"""
toy_dis.py — Disassembler for the ToyGPU ISA (Python implementation).

Usage: python toy_dis.py <input.bin>

Table-driven decoder using the shared isa_table.py.
"""

import sys
import struct
from isa_table import (
    find_by_mask, extract_field, sign_extend,
    FIELD_PN, FIELD_PRED, FIELD_RD, FIELD_RS1, FIELD_RS2, FIELD_FUNC,
    FIELD_IMM12, FIELD_RS1_B, FIELD_OFFSET17,
    FMT_R, FMT_I, FMT_B, FMT_L,
)


class DecodedInst:
    """Decoded instruction representation."""
    def __init__(self):
        self.address = 0
        self.mnemonic = ""
        self.pred_reg = 0
        self.pred_negate = False
        self.rd = -1
        self.rs1 = -1
        self.rs2 = -1
        self.immediate = 0
        self.raw_word0 = 0
        self.raw_word1 = 0
        self.is_two_word = False
        self.is_branch = False
        self.is_call = False
        self.is_return = False
        self.is_halt = False
        self.is_barrier = False
        self.is_unknown = False
        self.fmt = FMT_R

    def branch_target(self):
        return (self.address + 4 + self.immediate * 4) & 0xFFFFFFFF

    def size(self):
        return 8 if self.is_two_word else 4


def decode_one(words, index, base_address=0):
    """Decode one instruction. Returns (DecodedInst, new_index)."""
    inst = DecodedInst()
    inst.address = base_address + index * 4
    inst.raw_word0 = words[index]
    w = words[index]

    inst.pred_negate = bool(extract_field(w, FIELD_PN))
    inst.pred_reg = extract_field(w, FIELD_PRED)

    entry = find_by_mask(w)
    if entry is None:
        inst.mnemonic = ".unknown"
        inst.is_unknown = True
        return inst, index + 1

    inst.mnemonic = entry.mnemonic
    inst.fmt = entry.fmt
    inst.is_branch = entry.is_branch
    inst.is_call = entry.is_call
    inst.is_return = entry.is_return
    inst.is_halt = entry.is_halt
    inst.is_barrier = entry.is_barrier
    inst.is_two_word = entry.is_two_word

    if entry.fmt == FMT_R:
        inst.rd = extract_field(w, FIELD_RD)
        inst.rs1 = extract_field(w, FIELD_RS1)
        inst.rs2 = extract_field(w, FIELD_RS2)

    elif entry.fmt == FMT_I:
        inst.rd = extract_field(w, FIELD_RD)
        inst.rs1 = extract_field(w, FIELD_RS1)
        raw_imm = extract_field(w, FIELD_IMM12)
        if entry.imm_unsigned:
            inst.immediate = raw_imm
        else:
            inst.immediate = sign_extend(raw_imm, 12)

    elif entry.fmt == FMT_B:
        inst.rs1 = extract_field(w, FIELD_RS1_B)
        raw_off = extract_field(w, FIELD_OFFSET17)
        inst.immediate = sign_extend(raw_off, 17)

    elif entry.fmt == FMT_L:
        inst.rd = extract_field(w, FIELD_RD)
        if index + 1 >= len(words):
            inst.mnemonic = ".unknown"
            inst.is_unknown = True
            inst.is_two_word = False
            return inst, index + 1
        inst.raw_word1 = words[index + 1]
        inst.immediate = words[index + 1]
        # Treat as signed 32-bit for display
        if inst.immediate >= 0x80000000:
            inst.immediate = inst.immediate - 0x100000000

    new_index = index + (2 if entry.is_two_word else 1)
    return inst, new_index


def decode_all(words, base_address=0):
    """Decode all words into a list of DecodedInst."""
    result = []
    index = 0
    while index < len(words):
        inst, index = decode_one(words, index, base_address)
        result.append(inst)
    return result


def format_inst(inst):
    """Format a decoded instruction as a human-readable string."""
    parts = [f"0x{inst.address:04x}:  "]

    if inst.pred_reg != 0:
        parts.append("@")
        if inst.pred_negate:
            parts.append("!")
        parts.append(f"P{inst.pred_reg} ")

    parts.append(inst.mnemonic)

    if inst.is_unknown:
        parts.append(f" 0x{inst.raw_word0:08x}")
        return ''.join(parts)

    if inst.fmt == FMT_R:
        if inst.mnemonic in ('NOP', 'HALT', 'RET'):
            pass  # No operands printed
        elif inst.mnemonic.startswith('SETP'):
            parts.append(f" P{inst.rd & 7}, R{inst.rs1}, R{inst.rs2}")
        elif inst.mnemonic == 'BAR':
            parts.append(f" R{inst.rs1}")
        else:
            parts.append(f" R{inst.rd}, R{inst.rs1}, R{inst.rs2}")

    elif inst.fmt == FMT_I:
        if inst.mnemonic in ('LD', 'LDB', 'ST', 'STB'):
            off = ""
            if inst.immediate != 0:
                off = f"{inst.immediate:+d}"
            parts.append(f" R{inst.rd}, [R{inst.rs1}{off}]")
        else:
            parts.append(f" R{inst.rd}, R{inst.rs1}, {inst.immediate}")

    elif inst.fmt == FMT_B:
        if inst.is_return:
            pass
        else:
            if inst.mnemonic not in ('BRA',):
                parts.append(f" R{inst.rs1},")
            parts.append(f" 0x{inst.branch_target():04x}")

    elif inst.fmt == FMT_L:
        parts.append(f" R{inst.rd}, 0x{inst.immediate & 0xFFFFFFFF:08x}")

    return ''.join(parts)


def main():
    if len(sys.argv) < 2:
        print("Usage: python toy_dis.py <input.bin>", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    # Read complete 32-bit words, discarding any trailing partial word.
    # Note: len(data) - 3 would go negative for short data; use integer
    # division to compute the valid range safely.
    words = []
    num_words = len(data) // 4
    for i in range(num_words):
        words.append(struct.unpack('<I', data[i*4:(i+1)*4])[0])

    for inst in decode_all(words):
        print(format_inst(inst))


if __name__ == '__main__':
    main()
