"""
ptx_tool.py — CLI for PTX analysis tools.

Subcommands:
  parse   - Parse PTX and print structured IR
  cfg     - Build and print control-flow graph
  stats   - Print per-function statistics
  dot     - Generate Graphviz DOT file

Usage: python ptx_tool.py <subcommand> [options] <input.ptx>
"""

import argparse
import sys
import os

# Allow importing from same directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ptx_parser import parse_ptx
from ptx_cfg import build_cfg, compute_stats, format_stats, cfg_to_dot


def cmd_parse(args):
    """Parse PTX and print structured IR."""
    with open(args.input, 'r') as f:
        source = f.read()
    module = parse_ptx(source)

    print(f"PTX Version: {module.version}")
    print(f"Target: {module.target}")
    print(f"Address Size: {module.address_size}")
    print(f"Functions: {len(module.functions)}")

    for func in module.functions:
        kind = "kernel" if func.is_entry else "function"
        print(f"\n{'='*60}")
        print(f"{kind}: {func.name}")
        print(f"  Parameters: {len(func.params)}")
        print(f"  Instructions: {len(func.instructions)}")
        print(f"  Labels: {func.labels}")
        if args.verbose:
            for inst in func.instructions:
                pred = f"{inst.predicate} " if inst.predicate else ""
                lbl = f"{inst.label}: " if inst.label else "  "
                print(f"    {lbl}{pred}{inst.opcode} {', '.join(o.text for o in inst.operands)}")


def cmd_cfg(args):
    """Build and print CFG."""
    with open(args.input, 'r') as f:
        source = f.read()
    module = parse_ptx(source)

    for func in module.functions:
        cfg = build_cfg(func)
        print(f"\n=== CFG: {func.name} ===")
        print(f"Blocks: {len(cfg.blocks)}")
        print(f"Back edges (loops): {len(cfg.back_edges)}")
        for be in cfg.back_edges:
            print(f"  {be[0]} -> {be[1]}")
        print()
        for label, block in cfg.blocks.items():
            marker = " [ENTRY]" if block.is_entry else ""
            marker += " [EXIT]" if block.is_exit else ""
            print(f"  {label}{marker}:")
            for inst in block.instructions:
                print(f"    {inst.raw_text}")
            if block.successors:
                print(f"    -> {', '.join(block.successors)}")
            print()


def cmd_stats(args):
    """Print per-function statistics."""
    with open(args.input, 'r') as f:
        source = f.read()
    module = parse_ptx(source)

    for func in module.functions:
        cfg = build_cfg(func)
        stats = compute_stats(func, cfg)
        print(format_stats(stats))
        print()


def cmd_dot(args):
    """Generate Graphviz DOT file."""
    with open(args.input, 'r') as f:
        source = f.read()
    module = parse_ptx(source)

    for func in module.functions:
        cfg = build_cfg(func)
        dot = cfg_to_dot(cfg)

        if args.output:
            out_path = args.output
        else:
            base = os.path.splitext(args.input)[0]
            out_path = f"{base}_{func.name}.dot"

        with open(out_path, 'w') as f:
            f.write(dot)
        print(f"DOT written to {out_path}")


def main():
    parser = argparse.ArgumentParser(description="PTX Analysis Tool")
    subparsers = parser.add_subparsers(dest='command', help='Subcommand')

    # parse
    p_parse = subparsers.add_parser('parse', help='Parse PTX and print IR')
    p_parse.add_argument('input', help='Input PTX file')
    p_parse.add_argument('-v', '--verbose', action='store_true')

    # cfg
    p_cfg = subparsers.add_parser('cfg', help='Build and print CFG')
    p_cfg.add_argument('input', help='Input PTX file')

    # stats
    p_stats = subparsers.add_parser('stats', help='Print function statistics')
    p_stats.add_argument('input', help='Input PTX file')

    # dot
    p_dot = subparsers.add_parser('dot', help='Generate Graphviz DOT')
    p_dot.add_argument('input', help='Input PTX file')
    p_dot.add_argument('-o', '--output', help='Output DOT file')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    commands = {'parse': cmd_parse, 'cfg': cmd_cfg, 'stats': cmd_stats, 'dot': cmd_dot}
    try:
        commands[args.command](args)
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
