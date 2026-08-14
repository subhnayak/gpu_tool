"""
test_ptx.py — Tests for the PTX parser and CFG builder.

Run: pytest test_ptx.py -v
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'python'))

from ptx_parser import parse_ptx
from ptx_cfg import build_cfg, compute_stats, cfg_to_dot

SAMPLE_PTX = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sample.ptx')


def load_sample():
    with open(SAMPLE_PTX, 'r') as f:
        return parse_ptx(f.read())


class TestParser:
    def test_module_header(self):
        module = load_sample()
        assert module.version == "7.8"
        assert module.target == "sm_86"
        assert module.address_size == 64

    def test_function_found(self):
        module = load_sample()
        assert len(module.functions) >= 1
        func = module.functions[0]
        assert func.name == "vector_reduce"
        assert func.is_entry

    def test_parameters(self):
        module = load_sample()
        func = module.functions[0]
        assert len(func.params) >= 2

    def test_instructions_parsed(self):
        module = load_sample()
        func = module.functions[0]
        assert len(func.instructions) > 10
        # Check that we found branches
        branches = [i for i in func.instructions if i.is_branch]
        assert len(branches) >= 1

    def test_labels_found(self):
        module = load_sample()
        func = module.functions[0]
        assert "LOOP_HEAD" in func.labels
        assert "LOOP_END" in func.labels
        assert "EARLY_EXIT" in func.labels

    def test_predicates_parsed(self):
        module = load_sample()
        func = module.functions[0]
        preds = [i for i in func.instructions if i.predicate]
        assert len(preds) >= 2

    def test_registers_parsed(self):
        module = load_sample()
        func = module.functions[0]
        assert 'pred' in func.registers or 'b32' in func.registers

    def test_barriers_found(self):
        module = load_sample()
        func = module.functions[0]
        barriers = [i for i in func.instructions if i.is_barrier]
        assert len(barriers) >= 1


class TestCFG:
    def test_blocks_created(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        assert len(cfg.blocks) >= 3  # At minimum: entry, loop, exit

    def test_loop_detected(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        assert len(cfg.back_edges) >= 1, "Should detect at least one loop"

    def test_entry_block(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        entry = cfg.blocks.get(cfg.entry_label)
        assert entry is not None
        assert entry.is_entry

    def test_dominators_computed(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        # Entry dominates itself
        assert cfg.entry_label in cfg.dominators.get(cfg.entry_label, set())


class TestStats:
    def test_stats_computed(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        stats = compute_stats(func, cfg)
        assert stats.total_instructions > 0
        assert stats.branch_count >= 1
        assert stats.memory_op_count >= 1
        assert stats.barrier_count >= 1
        assert stats.block_count >= 3


class TestDOT:
    def test_dot_output(self):
        module = load_sample()
        func = module.functions[0]
        cfg = build_cfg(func)
        dot = cfg_to_dot(cfg)
        assert 'digraph' in dot
        assert func.name in dot
        assert '->' in dot
