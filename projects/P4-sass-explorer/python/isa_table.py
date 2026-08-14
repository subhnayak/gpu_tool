"""
isa_table.py — Single source of truth for the ToyGPU ISA (Python mirror).

This is the Python equivalent of include/toy_isa.h. In a real workflow,
BOTH this file and the C++ header would be auto-generated from one
machine-readable spec (JSON, YAML, or XML). That way, adding an instruction
means editing ONE file (the spec), and code-generation produces both
language-specific tables. The lesson: never maintain parallel tables by hand.

The table format matches the C++ version:
  - Each entry has a mnemonic, opcode, func, format, mask/match, and flags.
  - The assembler looks up by mnemonic; the disassembler looks up by mask/match.
"""

from dataclasses import dataclass
from typing import List, Optional

# Instruction formats
FMT_R = "R"  # Register-Register ALU
FMT_I = "I"  # Immediate
FMT_B = "B"  # Branch
FMT_L = "L"  # Long immediate (two-word)

# Field specifications: (offset, width, is_signed)
FIELD_PN      = (31, 1, False)
FIELD_PRED    = (28, 3, False)
FIELD_OPCODE  = (22, 6, False)
FIELD_RD      = (17, 5, False)
FIELD_RS1     = (12, 5, False)
FIELD_RS2     = (7,  5, False)
FIELD_FUNC    = (4,  3, False)
FIELD_IMM12   = (0,  12, True)
FIELD_RS1_B   = (17, 5, False)
FIELD_OFFSET17 = (0, 17, True)
FIELD_IMM_HI  = (0,  17, False)


def extract_field(word: int, field: tuple) -> int:
    """Extract a field from a 32-bit word."""
    offset, width, _ = field
    return (word >> offset) & ((1 << width) - 1)


def sign_extend(value: int, width: int) -> int:
    """Sign-extend a value of given bit-width to a Python int."""
    sign_bit = 1 << (width - 1)
    if value & sign_bit:
        return value - (1 << width)
    return value


def encode_field(word: int, field: tuple, value: int) -> int:
    """Encode a value into the given field of a 32-bit word."""
    offset, width, _ = field
    mask = (1 << width) - 1
    return word | ((value & mask) << offset)


@dataclass
class OpcodeEntry:
    mnemonic: str
    opcode: int
    func: int
    fmt: str  # FMT_R, FMT_I, FMT_B, FMT_L
    is_two_word: bool
    match_value: int
    mask: int
    imm_unsigned: bool = False
    is_branch: bool = False
    is_call: bool = False
    is_return: bool = False
    is_halt: bool = False
    is_barrier: bool = False
    sets_predicate: bool = False


def _r_match(op, fn):
    return (op << 22) | (fn << 4)

def _r_mask():
    return (0x3F << 22) | (0x7 << 4)

def _op_match(op):
    return op << 22

def _op_mask():
    return 0x3F << 22


OPCODE_TABLE: List[OpcodeEntry] = [
    # Format R ALU
    OpcodeEntry("ADD",  0x00, 0, FMT_R, False, _r_match(0x00,0), _r_mask()),
    OpcodeEntry("SUB",  0x00, 1, FMT_R, False, _r_match(0x00,1), _r_mask()),
    OpcodeEntry("AND",  0x00, 2, FMT_R, False, _r_match(0x00,2), _r_mask()),
    OpcodeEntry("OR",   0x00, 3, FMT_R, False, _r_match(0x00,3), _r_mask()),
    OpcodeEntry("XOR",  0x00, 4, FMT_R, False, _r_match(0x00,4), _r_mask()),
    OpcodeEntry("SHL",  0x00, 5, FMT_R, False, _r_match(0x00,5), _r_mask()),
    OpcodeEntry("SHR",  0x00, 6, FMT_R, False, _r_match(0x00,6), _r_mask()),
    OpcodeEntry("SLT",  0x00, 7, FMT_R, False, _r_match(0x00,7), _r_mask()),
    OpcodeEntry("SETP.EQ", 0x01, 0, FMT_R, False, _r_match(0x01,0), _r_mask(), sets_predicate=True),
    OpcodeEntry("SETP.NE", 0x01, 1, FMT_R, False, _r_match(0x01,1), _r_mask(), sets_predicate=True),
    OpcodeEntry("SETP.LT", 0x01, 2, FMT_R, False, _r_match(0x01,2), _r_mask(), sets_predicate=True),
    OpcodeEntry("SETP.GE", 0x01, 3, FMT_R, False, _r_match(0x01,3), _r_mask(), sets_predicate=True),
    OpcodeEntry("MUL",  0x02, 0, FMT_R, False, _r_match(0x02,0), _r_mask()),
    # Format I
    OpcodeEntry("ADDI", 0x08, 0, FMT_I, False, _op_match(0x08), _op_mask()),
    OpcodeEntry("ANDI", 0x09, 0, FMT_I, False, _op_match(0x09), _op_mask(), imm_unsigned=True),
    OpcodeEntry("ORI",  0x0A, 0, FMT_I, False, _op_match(0x0A), _op_mask(), imm_unsigned=True),
    OpcodeEntry("SLTI", 0x0B, 0, FMT_I, False, _op_match(0x0B), _op_mask()),
    OpcodeEntry("LD",   0x10, 0, FMT_I, False, _op_match(0x10), _op_mask()),
    OpcodeEntry("ST",   0x11, 0, FMT_I, False, _op_match(0x11), _op_mask()),
    OpcodeEntry("LDB",  0x12, 0, FMT_I, False, _op_match(0x12), _op_mask()),
    OpcodeEntry("STB",  0x13, 0, FMT_I, False, _op_match(0x13), _op_mask()),
    # Format B
    OpcodeEntry("BRA",  0x18, 0, FMT_B, False, _op_match(0x18), _op_mask(), is_branch=True),
    OpcodeEntry("BEZ",  0x19, 0, FMT_B, False, _op_match(0x19), _op_mask(), is_branch=True),
    OpcodeEntry("BNZ",  0x1A, 0, FMT_B, False, _op_match(0x1A), _op_mask(), is_branch=True),
    OpcodeEntry("CALL", 0x1B, 0, FMT_B, False, _op_match(0x1B), _op_mask(), is_branch=True, is_call=True),
    OpcodeEntry("RET",  0x1C, 0, FMT_B, False, _op_match(0x1C), _op_mask(), is_return=True),
    # Format L
    OpcodeEntry("LIMM", 0x20, 0, FMT_L, True, _op_match(0x20), _op_mask()),
    # Special
    OpcodeEntry("NOP",  0x3F, 0, FMT_R, False, _r_match(0x3F,0), _r_mask()),
    OpcodeEntry("HALT", 0x3E, 0, FMT_R, False, _r_match(0x3E,0), _r_mask(), is_halt=True),
    OpcodeEntry("BAR",  0x3D, 0, FMT_R, False, _r_match(0x3D,0), _r_mask(), is_barrier=True),
]


def find_by_mask(word: int) -> Optional[OpcodeEntry]:
    """Find the opcode entry matching a 32-bit word by mask/match."""
    for entry in OPCODE_TABLE:
        if (word & entry.mask) == entry.match_value:
            return entry
    return None


def find_by_mnemonic(mnem: str) -> Optional[OpcodeEntry]:
    """Find the opcode entry by mnemonic (case-insensitive)."""
    mnem_upper = mnem.upper()
    for entry in OPCODE_TABLE:
        if entry.mnemonic == mnem_upper:
            return entry
    return None
