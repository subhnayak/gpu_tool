"""
toy_asm.py — Assembler for the ToyGPU ISA (Python implementation).

Usage: python toy_asm.py input.asm output.bin

Two-pass assembler driven by the shared isa_table.py.
"""

import sys
import struct
import re
from isa_table import (
    OPCODE_TABLE, find_by_mnemonic,
    encode_field, FIELD_PN, FIELD_PRED, FIELD_OPCODE,
    FIELD_RD, FIELD_RS1, FIELD_RS2, FIELD_FUNC,
    FIELD_IMM12, FIELD_RS1_B, FIELD_OFFSET17,
    FMT_R, FMT_I, FMT_B, FMT_L,
)


def parse_register(s):
    """Parse 'R5' or 'r31', return int or None."""
    m = re.match(r'^[Rr](\d+)$', s)
    if not m:
        return None
    n = int(m.group(1))
    return n if 0 <= n <= 31 else None


def parse_pred_register(s):
    """Parse 'P3' or 'p7', return int or None."""
    m = re.match(r'^[Pp](\d+)$', s)
    if not m:
        return None
    n = int(m.group(1))
    return n if 0 <= n <= 7 else None


def parse_immediate(s):
    """Parse decimal or 0xHex integer, return int or None."""
    try:
        if s.lower().startswith('0x'):
            return int(s, 16)
        return int(s)
    except ValueError:
        return None


def parse_memory_operand(s):
    """Parse '[R5+4]' or '[R5-8]' or '[R5]'. Returns (base_reg, offset) or None."""
    m = re.match(r'^\[([Rr]\d+)([+-]\d+)?\]$', s)
    if not m:
        return None
    base_reg = parse_register(m.group(1))
    if base_reg is None:
        return None
    offset = int(m.group(2)) if m.group(2) else 0
    return (base_reg, offset)


def assemble(source):
    """
    Assemble source text into a list of 32-bit words.
    Returns (words, errors, labels).
    """
    lines = source.splitlines()
    errors = []
    labels = {}

    # Parse all lines
    parsed = []
    for i, raw_line in enumerate(lines, 1):
        line = raw_line.split(';')[0].split('//')[0].strip()
        if not line:
            parsed.append((i, None, None, None, []))
            continue

        label = None
        colon_pos = line.find(':')
        if colon_pos != -1:
            maybe_label = line[:colon_pos].strip()
            if ' ' not in maybe_label and maybe_label:
                label = maybe_label
                line = line[colon_pos+1:].strip()

        if not line:
            parsed.append((i, label, None, None, []))
            continue

        tokens = re.split(r'[,\s]+', line)
        tokens = [t for t in tokens if t]

        pred = None
        idx = 0
        if tokens and tokens[0].startswith('@'):
            pred = tokens[0]
            idx = 1

        mnemonic = tokens[idx].upper() if idx < len(tokens) else None
        idx += 1

        # Re-parse operands preserving [...] groups
        rest = line
        # Remove pred and mnemonic from rest
        if pred:
            rest = rest[len(pred):].strip()
        if mnemonic:
            # Find mnemonic in rest and skip it
            m_pos = rest.upper().find(mnemonic)
            if m_pos != -1:
                rest = rest[m_pos + len(mnemonic):].strip()

        operands = []
        if rest:
            # Split by comma, preserving brackets
            current = ''
            depth = 0
            for c in rest:
                if c == '[':
                    depth += 1
                elif c == ']':
                    depth -= 1
                if c == ',' and depth == 0:
                    t = current.strip()
                    if t:
                        operands.append(t)
                    current = ''
                else:
                    if c not in (' ', '\t') or depth > 0:
                        current += c
            t = current.strip()
            if t:
                operands.append(t)

        parsed.append((i, label, pred, mnemonic, operands))

    # Pass 1: collect labels
    current_addr = 0
    addr_map = []  # (line_num, address, entry_or_none)
    for (line_num, label, pred, mnemonic, operands) in parsed:
        if label:
            if label in labels:
                errors.append((line_num, f"duplicate label: {label}"))
            else:
                labels[label] = current_addr

        entry = None
        if mnemonic:
            entry = find_by_mnemonic(mnemonic)
            if not entry:
                errors.append((line_num, f"unknown mnemonic: {mnemonic}"))
            else:
                current_addr += 8 if entry.is_two_word else 4

        addr_map.append((line_num, current_addr - (8 if entry and entry.is_two_word else 4) if entry else current_addr, entry))

    if errors:
        return ([], errors, labels)

    # Pass 2: encode
    words = []
    for idx, (line_num, label, pred, mnemonic, operands) in enumerate(parsed):
        _, inst_addr, entry = addr_map[idx]
        if not mnemonic or not entry:
            continue

        word = 0

        # Predicate
        pred_reg = 0
        pred_neg = False
        if pred:
            p = pred[1:]  # skip @
            if p.startswith('!'):
                pred_neg = True
                p = p[1:]
            pr = parse_pred_register(p)
            if pr is None:
                errors.append((line_num, f"invalid predicate: {pred}"))
                continue
            pred_reg = pr

        word = encode_field(word, FIELD_PN, 1 if pred_neg else 0)
        word = encode_field(word, FIELD_PRED, pred_reg)
        word = encode_field(word, FIELD_OPCODE, entry.opcode)

        if entry.fmt == FMT_R:
            word = encode_field(word, FIELD_FUNC, entry.func)
            if entry.mnemonic in ('NOP', 'HALT'):
                pass
            elif entry.mnemonic == 'BAR':
                rs1 = parse_register(operands[0]) if operands else None
                if rs1 is None:
                    errors.append((line_num, "BAR requires register operand"))
                    continue
                word = encode_field(word, FIELD_RS1, rs1)
            elif entry.sets_predicate:
                if len(operands) < 3:
                    errors.append((line_num, f"{entry.mnemonic} requires 3 operands"))
                    continue
                pd = parse_pred_register(operands[0])
                rs1 = parse_register(operands[1])
                rs2 = parse_register(operands[2])
                if pd is None or rs1 is None or rs2 is None:
                    errors.append((line_num, "invalid operands"))
                    continue
                word = encode_field(word, FIELD_RD, pd)
                word = encode_field(word, FIELD_RS1, rs1)
                word = encode_field(word, FIELD_RS2, rs2)
            else:
                if len(operands) < 3:
                    errors.append((line_num, f"{entry.mnemonic} requires 3 operands"))
                    continue
                rd = parse_register(operands[0])
                rs1 = parse_register(operands[1])
                rs2 = parse_register(operands[2])
                if rd is None or rs1 is None or rs2 is None:
                    errors.append((line_num, "invalid register operands"))
                    continue
                word = encode_field(word, FIELD_RD, rd)
                word = encode_field(word, FIELD_RS1, rs1)
                word = encode_field(word, FIELD_RS2, rs2)

        elif entry.fmt == FMT_I:
            is_ls = entry.mnemonic in ('LD', 'ST', 'LDB', 'STB')
            if is_ls:
                if len(operands) < 2:
                    errors.append((line_num, f"{entry.mnemonic} requires 2 operands"))
                    continue
                rd = parse_register(operands[0])
                mem = parse_memory_operand(operands[1])
                if rd is None or mem is None:
                    errors.append((line_num, "invalid load/store operands"))
                    continue
                base_reg, offset = mem
                word = encode_field(word, FIELD_RD, rd)
                word = encode_field(word, FIELD_RS1, base_reg)
                word = encode_field(word, FIELD_IMM12, offset & 0xFFF)
            else:
                if len(operands) < 3:
                    errors.append((line_num, f"{entry.mnemonic} requires 3 operands"))
                    continue
                rd = parse_register(operands[0])
                rs1 = parse_register(operands[1])
                imm = parse_immediate(operands[2])
                if rd is None or rs1 is None or imm is None:
                    errors.append((line_num, "invalid operands"))
                    continue
                word = encode_field(word, FIELD_RD, rd)
                word = encode_field(word, FIELD_RS1, rs1)
                word = encode_field(word, FIELD_IMM12, imm & 0xFFF)

        elif entry.fmt == FMT_B:
            if entry.is_return:
                pass  # No operands
            else:
                is_uncond = entry.mnemonic in ('BRA', 'CALL')
                expected = 1 if is_uncond else 2
                if len(operands) < expected:
                    errors.append((line_num, f"{entry.mnemonic} requires {expected} operand(s)"))
                    continue

                target_idx = expected - 1
                if not is_uncond:
                    rs1 = parse_register(operands[0])
                    if rs1 is None:
                        errors.append((line_num, "invalid register"))
                        continue
                    word = encode_field(word, FIELD_RS1_B, rs1)

                target_str = operands[target_idx]
                imm = parse_immediate(target_str)
                if imm is not None:
                    offset_words = (imm - (inst_addr + 4)) // 4
                elif target_str in labels:
                    offset_words = (labels[target_str] - (inst_addr + 4)) // 4
                else:
                    errors.append((line_num, f"undefined label: {target_str}"))
                    continue
                word = encode_field(word, FIELD_OFFSET17, offset_words & 0x1FFFF)

        elif entry.fmt == FMT_L:
            if len(operands) < 2:
                errors.append((line_num, "LIMM requires 2 operands"))
                continue
            rd = parse_register(operands[0])
            imm = parse_immediate(operands[1])
            if rd is None or imm is None:
                errors.append((line_num, "invalid LIMM operands"))
                continue
            word = encode_field(word, FIELD_RD, rd)
            words.append(word & 0xFFFFFFFF)
            words.append(imm & 0xFFFFFFFF)
            continue

        words.append(word & 0xFFFFFFFF)

    return (words, errors, labels)


def main():
    if len(sys.argv) < 3:
        print("Usage: python toy_asm.py <input.asm> <output.bin>", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        source = f.read()

    words, errors, labels = assemble(source)

    if errors:
        for line_num, msg in errors:
            print(f"{sys.argv[1]}:{line_num}: error: {msg}", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[2], 'wb') as f:
        for w in words:
            f.write(struct.pack('<I', w))

    print(f"Assembled {len(words)} words ({len(words)*4} bytes)")


if __name__ == '__main__':
    main()
