"""
ptx_cfg.py — Control-flow graph reconstruction for parsed PTX.

Builds CFGs per function, detects loops via DFS, computes dominators
with a simple iterative algorithm, and generates statistics and DOT output.
"""

from dataclasses import dataclass, field
from typing import Dict, List, Set, Tuple, Optional
from ptx_parser import PTXFunction, PTXInstruction, PTXModule


@dataclass
class PTXBlock:
    """A basic block in a PTX CFG."""
    label: str
    instructions: List[PTXInstruction] = field(default_factory=list)
    successors: List[str] = field(default_factory=list)
    predecessors: List[str] = field(default_factory=list)
    is_entry: bool = False
    is_exit: bool = False


@dataclass
class PTXFunctionCFG:
    """CFG for a single PTX function."""
    function_name: str
    blocks: Dict[str, PTXBlock] = field(default_factory=dict)
    entry_label: str = ""
    back_edges: List[Tuple[str, str]] = field(default_factory=list)
    dominators: Dict[str, Set[str]] = field(default_factory=dict)


@dataclass
class FunctionStats:
    """Per-function statistics."""
    name: str = ""
    total_instructions: int = 0
    instruction_mix: Dict[str, int] = field(default_factory=dict)
    register_types: Dict[str, int] = field(default_factory=dict)
    branch_count: int = 0
    memory_op_count: int = 0
    barrier_count: int = 0
    block_count: int = 0
    loop_count: int = 0
    estimated_register_pressure: int = 0


def build_cfg(func: PTXFunction) -> PTXFunctionCFG:
    """Build a CFG from a parsed PTX function."""
    cfg = PTXFunctionCFG(function_name=func.name)

    if not func.instructions:
        return cfg

    # Step 1: identify leaders (basic block starts)
    leaders = set()
    leaders.add(0)  # First instruction

    for i, inst in enumerate(func.instructions):
        if inst.label:
            leaders.add(i)
        if inst.is_branch or inst.is_return:
            if i + 1 < len(func.instructions):
                leaders.add(i + 1)

    # Step 2: build blocks
    leader_list = sorted(leaders)
    label_to_block = {}  # map PTX labels to block labels

    for li, start in enumerate(leader_list):
        end = leader_list[li + 1] if li + 1 < len(leader_list) else len(func.instructions)
        block_insts = func.instructions[start:end]

        # Determine block label
        if block_insts and block_insts[0].label:
            block_label = block_insts[0].label
        else:
            block_label = f"BB_{start}"

        block = PTXBlock(label=block_label, instructions=block_insts)
        block.is_entry = (start == 0)
        cfg.blocks[block_label] = block

        # Map all labels in this block to the block label
        for inst in block_insts:
            if inst.label:
                label_to_block[inst.label] = block_label

    if cfg.blocks:
        cfg.entry_label = list(cfg.blocks.keys())[0]

    # Step 3: add edges
    block_labels = list(cfg.blocks.keys())
    for i, blabel in enumerate(block_labels):
        block = cfg.blocks[blabel]
        if not block.instructions:
            continue

        last = block.instructions[-1]
        falls_through = True

        if last.is_return:
            block.is_exit = True
            falls_through = False

        if last.is_branch:
            # Find branch target
            for op in last.operands:
                if op.is_label and op.text in label_to_block:
                    target = label_to_block[op.text]
                    block.successors.append(target)
                    cfg.blocks[target].predecessors.append(blabel)

            # Unconditional branch with no predicate doesn't fall through
            if not last.predicate and last.is_branch:
                falls_through = False

        if falls_through and i + 1 < len(block_labels):
            next_label = block_labels[i + 1]
            block.successors.append(next_label)
            cfg.blocks[next_label].predecessors.append(blabel)

    # Step 4: detect loops (back edges via DFS)
    _detect_loops(cfg)

    # Step 5: compute dominators
    _compute_dominators(cfg)

    return cfg


def _detect_loops(cfg: PTXFunctionCFG):
    """Detect back edges indicating loops using DFS."""
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {label: WHITE for label in cfg.blocks}

    def dfs(u):
        color[u] = GRAY
        for v in cfg.blocks[u].successors:
            if v not in color:
                continue
            if color[v] == GRAY:
                cfg.back_edges.append((u, v))
            elif color[v] == WHITE:
                dfs(v)
        color[u] = BLACK

    if cfg.entry_label in cfg.blocks:
        dfs(cfg.entry_label)


def _compute_dominators(cfg: PTXFunctionCFG):
    """
    Compute dominators using the simple iterative algorithm.

    Dom(entry) = {entry}
    Dom(n) = {n} ∪ (∩ Dom(p) for all predecessors p of n)

    Iterate until no changes. O(n²) per iteration, usually converges fast.
    """
    all_nodes = set(cfg.blocks.keys())
    dom = {}

    for n in all_nodes:
        if n == cfg.entry_label:
            dom[n] = {n}
        else:
            dom[n] = set(all_nodes)  # Start with all nodes

    changed = True
    while changed:
        changed = False
        for n in all_nodes:
            if n == cfg.entry_label:
                continue
            preds = cfg.blocks[n].predecessors
            if preds:
                new_dom = set.intersection(*(dom.get(p, all_nodes) for p in preds))
            else:
                new_dom = set()
            new_dom = {n} | new_dom
            if new_dom != dom[n]:
                dom[n] = new_dom
                changed = True

    cfg.dominators = dom


def compute_stats(func: PTXFunction, cfg: PTXFunctionCFG) -> FunctionStats:
    """Compute per-function statistics."""
    stats = FunctionStats(name=func.name)
    stats.register_types = dict(func.registers)
    stats.block_count = len(cfg.blocks)
    stats.loop_count = len(cfg.back_edges)

    for inst in func.instructions:
        if not inst.opcode:
            continue  # label-only
        stats.total_instructions += 1

        # Instruction mix: count base opcode
        base = inst.opcode.split('.')[0]
        stats.instruction_mix[base] = stats.instruction_mix.get(base, 0) + 1

        if inst.is_branch:
            stats.branch_count += 1
        if inst.is_barrier:
            stats.barrier_count += 1
        if base in ('ld', 'st', 'atom', 'red'):
            stats.memory_op_count += 1

    # Estimate register pressure (sum of all register counts)
    stats.estimated_register_pressure = sum(func.registers.values())

    return stats


def cfg_to_dot(cfg: PTXFunctionCFG) -> str:
    """Generate Graphviz DOT representation."""
    lines = [f'digraph "{cfg.function_name}" {{']
    lines.append('  rankdir=TB;')
    lines.append('  node [shape=box, fontname="Courier"];')

    back_edge_set = set(cfg.back_edges)

    for label, block in cfg.blocks.items():
        # Build label text
        inst_texts = []
        for inst in block.instructions:
            t = inst.raw_text.replace('\\', '\\\\').replace('"', '\\"')
            if len(t) > 60:
                t = t[:57] + "..."
            inst_texts.append(t)
        node_label = '\\n'.join(inst_texts) if inst_texts else label
        attrs = f'label="{node_label}"'
        if block.is_entry:
            attrs += ', style=bold, color=green'
        if block.is_exit:
            attrs += ', style=bold, color=red'
        lines.append(f'  "{label}" [{attrs}];')

    for label, block in cfg.blocks.items():
        for succ in block.successors:
            edge_attrs = ''
            if (label, succ) in back_edge_set:
                edge_attrs = ' [style=dashed, color=blue, label="loop"]'
            lines.append(f'  "{label}" -> "{succ}"{edge_attrs};')

    lines.append('}')
    return '\n'.join(lines)


def format_stats(stats: FunctionStats) -> str:
    """Format statistics as human-readable text."""
    lines = [f"=== Function: {stats.name} ==="]
    lines.append(f"Total instructions: {stats.total_instructions}")
    lines.append(f"Basic blocks: {stats.block_count}")
    lines.append(f"Loops: {stats.loop_count}")
    lines.append(f"Branches: {stats.branch_count}")
    lines.append(f"Memory operations: {stats.memory_op_count}")
    lines.append(f"Barriers: {stats.barrier_count}")
    lines.append(f"Est. register pressure: {stats.estimated_register_pressure}")

    if stats.register_types:
        lines.append("\nRegister types:")
        for rtype, count in sorted(stats.register_types.items()):
            lines.append(f"  .{rtype}: {count}")

    if stats.instruction_mix:
        lines.append("\nInstruction mix:")
        for op, count in sorted(stats.instruction_mix.items(), key=lambda x: -x[1]):
            pct = 100 * count / stats.total_instructions if stats.total_instructions else 0
            lines.append(f"  {op:20s} {count:4d}  ({pct:.1f}%)")

    return '\n'.join(lines)
