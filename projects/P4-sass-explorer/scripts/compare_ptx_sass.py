"""
compare_ptx_sass.py — Line up PTX instructions against SASS for the same kernel.

Usage: python compare_ptx_sass.py <ptx_file> <sass_file>

This script attempts a best-effort alignment between PTX and SASS by:
1. Parsing both files to extract per-kernel instruction lists
2. Using line-info annotations (if compiled with -lineinfo) to correlate
3. Falling back to sequential alignment if no line info

LIMITATIONS (be honest about these in an interview!):
- PTX-to-SASS is a many-to-many mapping: one PTX instruction may become
  multiple SASS instructions (expansion), and the compiler may reorder,
  eliminate, or fuse instructions.
- Without -lineinfo, alignment is heuristic at best.
- The compiler may inline functions, unroll loops, and constant-fold,
  making the PTX structure unrecognizable in SASS.
- This tool is for LEARNING and EXPLORATION, not for production analysis.
  NVIDIA's own Nsight Compute provides proper source-to-SASS correlation.
"""

import re
import sys
import os


def parse_ptx_instructions(ptx_text):
    """Extract kernel name -> list of instruction lines from PTX."""
    kernels = {}
    current_kernel = None
    brace_depth = 0

    for line in ptx_text.splitlines():
        stripped = line.strip()

        # Find kernel entry
        m = re.match(r'\.(?:visible\s+)?\.entry\s+(\w+)', stripped)
        if m:
            current_kernel = m.group(1)
            kernels[current_kernel] = []
            continue

        if '{' in stripped:
            brace_depth += 1
        if '}' in stripped:
            brace_depth -= 1
            if brace_depth <= 0:
                current_kernel = None
                brace_depth = 0
            continue

        if current_kernel and stripped and not stripped.startswith('.'):
            # It's an instruction (or label)
            kernels[current_kernel].append(stripped.rstrip(';'))

    return kernels


def parse_sass_instructions(sass_text):
    """Extract kernel name -> list of SASS instruction lines."""
    kernels = {}
    current_kernel = None

    for line in sass_text.splitlines():
        # cuobjdump format: "Function : kernel_name"
        m = re.match(r'^\s*Function\s*:\s*(\w+)', line)
        if m:
            current_kernel = m.group(1)
            kernels[current_kernel] = []
            continue

        # nvdisasm format: ".text.kernel_name:"
        m = re.match(r'^\s*\.text\.(\w+):', line)
        if m:
            current_kernel = m.group(1)
            kernels[current_kernel] = []
            continue

        if current_kernel:
            # SASS instruction lines typically have hex addresses
            m = re.match(r'^\s*/\*\s*[0-9a-fA-F]+\s*\*/\s*(.*)', line)
            if m:
                kernels[current_kernel].append(m.group(1).strip())
                continue
            # nvdisasm format
            m = re.match(r'^\s*[0-9a-fA-F]+:\s*(.*)', line)
            if m and m.group(1).strip():
                kernels[current_kernel].append(m.group(1).strip())

    return kernels


def align_instructions(ptx_insts, sass_insts):
    """
    Best-effort sequential alignment of PTX and SASS instructions.

    This is a naive alignment: just interleave them proportionally.
    A real tool would use debug info (-lineinfo) for proper mapping.
    """
    if not ptx_insts or not sass_insts:
        return []

    result = []
    ratio = len(sass_insts) / len(ptx_insts) if ptx_insts else 1

    for i, ptx in enumerate(ptx_insts):
        # Find corresponding SASS range
        sass_start = int(i * ratio)
        sass_end = int((i + 1) * ratio)
        sass_end = min(sass_end, len(sass_insts))

        sass_group = sass_insts[sass_start:sass_end] if sass_start < len(sass_insts) else []
        result.append((ptx, sass_group))

    return result


def main():
    if len(sys.argv) < 3:
        print("Usage: python compare_ptx_sass.py <ptx_file> <sass_file>")
        print()
        print("Aligns PTX and SASS instructions for comparison.")
        print("Compile with -lineinfo for best results.")
        sys.exit(1)

    with open(sys.argv[1], 'r') as f:
        ptx_text = f.read()
    with open(sys.argv[2], 'r') as f:
        sass_text = f.read()

    ptx_kernels = parse_ptx_instructions(ptx_text)
    sass_kernels = parse_sass_instructions(sass_text)

    if not ptx_kernels:
        print("No kernels found in PTX file.")
        sys.exit(1)

    for kname in ptx_kernels:
        print(f"\n{'='*80}")
        print(f"Kernel: {kname}")
        print(f"{'='*80}")

        ptx_insts = ptx_kernels.get(kname, [])
        sass_insts = sass_kernels.get(kname, [])

        print(f"PTX instructions: {len(ptx_insts)}")
        print(f"SASS instructions: {len(sass_insts)}")

        if not sass_insts:
            print(f"  (No SASS found for this kernel — check SASS file)")
            print(f"\n  PTX only:")
            for inst in ptx_insts:
                print(f"    {inst}")
            continue

        expansion = len(sass_insts) / len(ptx_insts) if ptx_insts else 0
        print(f"Expansion ratio: {expansion:.1f}x")
        print(f"\nNOTE: This alignment is HEURISTIC. PTX and SASS do not have")
        print(f"a 1:1 correspondence. The compiler reorders, fuses, and splits")
        print(f"instructions. Use Nsight Compute for proper correlation.\n")

        aligned = align_instructions(ptx_insts, sass_insts)
        for ptx, sass_group in aligned:
            print(f"  PTX:  {ptx}")
            for s in sass_group:
                print(f"  SASS:   {s}")
            print()


if __name__ == '__main__':
    main()
