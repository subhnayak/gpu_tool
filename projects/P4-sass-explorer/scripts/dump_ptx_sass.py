"""
dump_ptx_sass.py — Extract PTX and SASS from compiled CUDA kernels.

Usage: python dump_ptx_sass.py <input.cu> [--output-dir <dir>] [--arch sm_86]

This script:
  1. Compiles <input.cu> to PTX with nvcc -ptx
  2. Compiles to cubin with nvcc -cubin
  3. Dumps SASS via cuobjdump -sass
  4. Dumps SASS via nvdisasm (if available)
  5. Saves all outputs to the output directory with a summary

Handles missing tools gracefully. Each step is independent; if cuobjdump
is not found, the PTX step still runs.
"""

import argparse
import os
import subprocess
import sys
import shutil
from datetime import datetime


def find_tool(name):
    """Find an executable in PATH."""
    return shutil.which(name)


def run_cmd(cmd, timeout=60, desc="command"):
    """Run a command, return (success, stdout, stderr)."""
    print(f"  Running: {' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        if result.returncode != 0:
            print(f"  WARNING: {desc} exited with code {result.returncode}")
            if result.stderr:
                print(f"  stderr: {result.stderr[:500]}")
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        print(f"  ERROR: {desc} timed out after {timeout}s")
        return False, "", "timeout"
    except FileNotFoundError:
        print(f"  ERROR: {desc} not found")
        return False, "", "not found"


def main():
    parser = argparse.ArgumentParser(description="Dump PTX and SASS from CUDA source")
    parser.add_argument('input', help='Input .cu file')
    parser.add_argument('--output-dir', '-o', default=None,
                       help='Output directory (default: <input>_dump)')
    parser.add_argument('--arch', '-a', default='sm_89',
                       help='GPU architecture (default: sm_89)')
    parser.add_argument('--lineinfo', action='store_true',
                       help='Include line info (-lineinfo flag)')
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    out_dir = args.output_dir or os.path.splitext(args.input)[0] + '_dump'
    os.makedirs(out_dir, exist_ok=True)

    base_name = os.path.splitext(os.path.basename(args.input))[0]
    summary_lines = [
        f"CUDA Dump Summary",
        f"Source: {os.path.abspath(args.input)}",
        f"Architecture: {args.arch}",
        f"Date: {datetime.now().isoformat()}",
        f""
    ]

    # Check for nvcc
    nvcc = find_tool('nvcc')
    if not nvcc:
        print("ERROR: nvcc not found in PATH.")
        print("Install CUDA Toolkit and ensure nvcc is on your PATH.")
        print("On Windows: typically C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\vX.Y\\bin")
        summary_lines.append("ERROR: nvcc not found")
    else:
        print(f"Found nvcc: {nvcc}")

        # Step 1: Generate PTX
        ptx_file = os.path.join(out_dir, f"{base_name}.ptx")
        cmd = ['nvcc', '-ptx', f'-arch={args.arch}']
        if args.lineinfo:
            cmd.append('-lineinfo')
        cmd.extend(['-o', ptx_file, args.input])
        ok, stdout, stderr = run_cmd(cmd, timeout=120, desc="nvcc -ptx")
        if ok:
            summary_lines.append(f"PTX: {ptx_file}")
            print(f"  PTX generated: {ptx_file}")
        else:
            summary_lines.append(f"PTX: FAILED - {stderr[:200]}")

        # Step 2: Generate cubin
        cubin_file = os.path.join(out_dir, f"{base_name}.cubin")
        cmd = ['nvcc', '-cubin', f'-arch={args.arch}']
        if args.lineinfo:
            cmd.append('-lineinfo')
        cmd.extend(['-o', cubin_file, args.input])
        ok, stdout, stderr = run_cmd(cmd, timeout=120, desc="nvcc -cubin")
        if ok:
            summary_lines.append(f"CUBIN: {cubin_file}")
            print(f"  cubin generated: {cubin_file}")

            # Step 3: cuobjdump -sass
            cuobjdump = find_tool('cuobjdump')
            if cuobjdump:
                sass_file = os.path.join(out_dir, f"{base_name}_cuobjdump.sass")
                cmd = ['cuobjdump', '-sass', cubin_file]
                ok, stdout, stderr = run_cmd(cmd, desc="cuobjdump -sass")
                if ok and stdout:
                    with open(sass_file, 'w') as f:
                        f.write(stdout)
                    summary_lines.append(f"SASS (cuobjdump): {sass_file}")
                    print(f"  SASS dumped: {sass_file}")
            else:
                print("  cuobjdump not found — skipping SASS dump via cuobjdump")
                summary_lines.append("SASS (cuobjdump): skipped (tool not found)")

            # Step 4: nvdisasm
            nvdisasm = find_tool('nvdisasm')
            if nvdisasm:
                sass_file2 = os.path.join(out_dir, f"{base_name}_nvdisasm.sass")
                cmd = ['nvdisasm', cubin_file]
                ok, stdout, stderr = run_cmd(cmd, desc="nvdisasm")
                if ok and stdout:
                    with open(sass_file2, 'w') as f:
                        f.write(stdout)
                    summary_lines.append(f"SASS (nvdisasm): {sass_file2}")
                    print(f"  SASS dumped: {sass_file2}")
            else:
                print("  nvdisasm not found — skipping")
                summary_lines.append("SASS (nvdisasm): skipped (tool not found)")
        else:
            summary_lines.append(f"CUBIN: FAILED")

    # Write summary
    summary_file = os.path.join(out_dir, "summary.txt")
    with open(summary_file, 'w') as f:
        f.write('\n'.join(summary_lines) + '\n')
    print(f"\nSummary written to {summary_file}")


if __name__ == '__main__':
    main()
