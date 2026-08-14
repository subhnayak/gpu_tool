"""
ptx_parser.py — Parser for NVIDIA PTX (Parallel Thread Execution) assembly.

PTX is the public virtual ISA for NVIDIA GPUs. Unlike SASS (the real machine
ISA), PTX is documented and can be generated from CUDA with `nvcc -ptx`.
This parser builds a structured IR from PTX text.

Design: tolerant parsing. Unknown directives or instructions are recorded
as-is, not treated as fatal errors. This makes the parser useful on PTX
from any nvcc version without requiring updates.
"""

import re
from dataclasses import dataclass, field
from typing import List, Optional, Dict


@dataclass
class PTXOperand:
    """A single operand in a PTX instruction."""
    text: str           # Raw operand text
    is_register: bool = False
    is_immediate: bool = False
    is_label: bool = False
    is_memory: bool = False
    register: str = ""  # e.g. "%r1", "%rd2"
    base_reg: str = ""  # For memory: [%rd2+4]
    offset: int = 0


@dataclass
class PTXInstruction:
    """A single PTX instruction."""
    address: int = 0    # Line number (PTX doesn't have byte addresses)
    predicate: str = "" # e.g. "@%p1", "@!%p2"
    opcode: str = ""    # e.g. "add.s32", "ld.global.f32"
    type_suffix: str = ""  # e.g. ".s32", ".f32"
    operands: List[PTXOperand] = field(default_factory=list)
    raw_text: str = ""
    is_branch: bool = False
    is_call: bool = False
    is_return: bool = False
    is_barrier: bool = False
    label: str = ""     # Label defined at this instruction, if any


@dataclass
class PTXBasicBlock:
    """A basic block in a PTX function."""
    label: str = ""
    instructions: List[PTXInstruction] = field(default_factory=list)


@dataclass
class PTXParam:
    """A kernel/function parameter."""
    name: str = ""
    type_str: str = ""
    raw: str = ""


@dataclass
class PTXFunction:
    """A PTX function/kernel."""
    name: str = ""
    is_entry: bool = False
    params: List[PTXParam] = field(default_factory=list)
    registers: Dict[str, int] = field(default_factory=dict)  # type -> count
    instructions: List[PTXInstruction] = field(default_factory=list)
    labels: List[str] = field(default_factory=list)
    raw_directives: List[str] = field(default_factory=list)


@dataclass
class PTXModule:
    """A complete PTX module (one .ptx file)."""
    version: str = ""
    target: str = ""
    address_size: int = 64
    functions: List[PTXFunction] = field(default_factory=list)
    globals: List[str] = field(default_factory=list)  # Global variable declarations
    raw_headers: List[str] = field(default_factory=list)


def parse_operand(text: str) -> PTXOperand:
    """Parse a single PTX operand."""
    text = text.strip()
    op = PTXOperand(text=text)

    # Memory operand: [%rd2+4] or [%rd2]
    m = re.match(r'^\[(%\w+)(?:\s*\+\s*(\d+))?\]$', text)
    if m:
        op.is_memory = True
        op.base_reg = m.group(1)
        op.offset = int(m.group(2)) if m.group(2) else 0
        return op

    # Register: %r1, %rd2, %p3, %f4
    if text.startswith('%'):
        op.is_register = True
        op.register = text
        return op

    # Immediate: numeric
    try:
        int(text, 0)
        op.is_immediate = True
        return op
    except ValueError:
        pass

    # Float immediate
    try:
        float(text)
        op.is_immediate = True
        return op
    except ValueError:
        pass

    # Label or symbol
    if re.match(r'^[A-Za-z_$][\w$]*$', text):
        op.is_label = True
        return op

    return op


def parse_ptx(source: str) -> PTXModule:
    """Parse PTX source text into a structured module."""
    module = PTXModule()
    lines = source.splitlines()

    current_function = None
    in_function_body = False
    brace_depth = 0

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1

        # Skip empty lines and comments
        if not line or line.startswith('//'):
            continue

        # Version directive
        m = re.match(r'\.version\s+(\S+)', line)
        if m:
            module.version = m.group(1)
            module.raw_headers.append(line)
            continue

        # Target directive
        m = re.match(r'\.target\s+(\S+)', line)
        if m:
            module.target = m.group(1)
            module.raw_headers.append(line)
            continue

        # Address size
        m = re.match(r'\.address_size\s+(\d+)', line)
        if m:
            module.address_size = int(m.group(1))
            module.raw_headers.append(line)
            continue

        # Function/kernel declaration — try several common PTX forms
        found_func = False
        is_entry = False
        fname = ""

        # Form 1: .visible .entry name  or  .visible .func name
        m = re.match(r'\.(?:visible\s+)?\.(?:entry|func)\s+(\w+)', line)
        if m:
            is_entry = '.entry' in line
            fname = m.group(1)
            found_func = True

        # Form 2: .entry name  (without dot-dot)
        if not found_func:
            m = re.match(r'\.(entry|func)\s+(\w+)', line)
            if m:
                is_entry = m.group(1) == 'entry'
                fname = m.group(2)
                found_func = True

        if found_func:
            current_function = PTXFunction(name=fname, is_entry=is_entry)
            module.functions.append(current_function)

            # Collect parameters (may span multiple lines until '{')
            while i < len(lines) and '{' not in lines[i-1]:
                param_line = lines[i].strip()
                i += 1
                if param_line.startswith('.param'):
                    pm = re.match(r'\.param\s+\.(\w+)\s+(\w+)', param_line)
                    if pm:
                        current_function.params.append(
                            PTXParam(name=pm.group(2), type_str=pm.group(1), raw=param_line))
                if '{' in param_line:
                    break

            in_function_body = True
            brace_depth = 1
            continue

        # Track braces
        if '{' in line:
            brace_depth += 1
            if not in_function_body and current_function:
                in_function_body = True
            continue

        if '}' in line:
            brace_depth -= 1
            if brace_depth <= 0:
                in_function_body = False
                current_function = None
                brace_depth = 0
            continue

        # Inside function body
        if in_function_body and current_function:
            # Register declaration: .reg .s32 %r<10>;
            m = re.match(r'\.reg\s+\.(\w+)\s+(%\w+)<(\d+)>', line.rstrip(';'))
            if m:
                reg_type = m.group(1)
                count = int(m.group(3))
                current_function.registers[reg_type] = max(
                    current_function.registers.get(reg_type, 0), count)
                current_function.raw_directives.append(line)
                continue

            # Other directives inside function
            if line.startswith('.'):
                current_function.raw_directives.append(line)
                continue

            # Label
            label = ""
            m = re.match(r'^(\w+):\s*(.*)', line)
            if m:
                label = m.group(1)
                current_function.labels.append(label)
                line = m.group(2).strip()
                if not line:
                    # Label-only line — create a placeholder
                    inst = PTXInstruction(address=i-1, label=label, raw_text=f"{label}:")
                    current_function.instructions.append(inst)
                    continue

            # Instruction
            inst = PTXInstruction(address=i-1, label=label, raw_text=line.rstrip(';'))

            # Check for predicate
            pm = re.match(r'^(@!?%\w+)\s+(.*)', line)
            if pm:
                inst.predicate = pm.group(1)
                line = pm.group(2)

            # Parse opcode and operands
            parts = line.rstrip(';').split(None, 1)
            if parts:
                inst.opcode = parts[0]
                # Extract type suffix
                dot_parts = inst.opcode.split('.')
                if len(dot_parts) > 1:
                    inst.type_suffix = '.' + dot_parts[-1]

                # Classify
                base_op = dot_parts[0]
                if base_op in ('bra', 'brx'):
                    inst.is_branch = True
                elif base_op == 'call':
                    inst.is_call = True
                elif base_op == 'ret':
                    inst.is_return = True
                elif base_op in ('bar', 'barrier', 'membar'):
                    inst.is_barrier = True

                # Parse operands
                if len(parts) > 1:
                    # Split by comma, respecting brackets
                    ops_text = parts[1].rstrip(';').strip()
                    ops = []
                    current = ''
                    depth = 0
                    for c in ops_text:
                        if c == '[':
                            depth += 1
                        elif c == ']':
                            depth -= 1
                        if c == ',' and depth == 0:
                            ops.append(current.strip())
                            current = ''
                        else:
                            current += c
                    if current.strip():
                        ops.append(current.strip())
                    inst.operands = [parse_operand(o) for o in ops]

            current_function.instructions.append(inst)
            continue

        # Global declarations
        if line.startswith('.') and not in_function_body:
            module.globals.append(line)
            continue

    return module
