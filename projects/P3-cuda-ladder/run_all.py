#!/usr/bin/env python3
"""
run_all.py — Build (optional) and run every CUDA Ladder rung, then emit a
consolidated Markdown results table.

Usage:
  python run_all.py                   # just run (assumes already built)
  python run_all.py --build           # cmake build first, then run
  python run_all.py --build-dir build # specify build directory

The script parses the printed lines from each executable looking for the
pattern:
    <name>   <time> ms   <value> GB/s   (<pct>% peak)
  or
    <name>   <time> ms   <value> GFLOP/s   (<pct>% peak)

It collects all such lines and prints a Markdown table at the end.
"""

import subprocess
import sys
import os
import re
import argparse

RUNGS = [
    "01_vector_add",
    "02_coalescing",
    "03_transpose",
    "04_reduction",
    "05_scan",
    "06_histogram",
    "07_sgemm",
    "08_radix_sort",
    "09_streams",
    "10_occupancy",
]

# Regex to match result lines from reportBandwidth / reportGFlops.
RESULT_RE = re.compile(
    r'^\s+'
    r'(?P<name>.+?)\s{2,}'           # kernel name (at least 2 spaces after)
    r'(?P<time>[\d.]+)\s+ms\s+'      # time in ms
    r'(?P<value>[\d.]+)\s+'          # throughput value
    r'(?P<unit>GB/s|GFLOP/s)\s+'     # unit
    r'\(\s*(?P<pct>[\d.]+)%\s+peak\)'  # % of peak
)


def find_executable(name, build_dir):
    """Find executable, handling Windows .exe and CMake build layouts."""
    candidates = [
        os.path.join(build_dir, name),
        os.path.join(build_dir, name + ".exe"),
        os.path.join(build_dir, "Debug", name + ".exe"),
        os.path.join(build_dir, "Release", name + ".exe"),
        os.path.join(".", name),
        os.path.join(".", name + ".exe"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def main():
    parser = argparse.ArgumentParser(description="Run all CUDA Ladder rungs")
    parser.add_argument("--build", action="store_true", help="Build before running")
    parser.add_argument("--build-dir", default="build", help="CMake build directory")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    if args.build:
        print("=== Building ===")
        os.makedirs(args.build_dir, exist_ok=True)
        subprocess.run(["cmake", "-B", args.build_dir, "-S", "."], check=True)
        subprocess.run(["cmake", "--build", args.build_dir, "--config", "Release"],
                       check=True)
        print()

    all_results = []

    for rung in RUNGS:
        exe = find_executable(rung, args.build_dir)
        if exe is None:
            print(f"--- {rung}: executable NOT FOUND, skipping ---")
            all_results.append((rung, None, None, None, None, "NOT FOUND"))
            continue

        print(f"=== Running {rung} ===")
        try:
            result = subprocess.run(
                [exe], capture_output=True, text=True, timeout=300
            )
            output = result.stdout + result.stderr
            print(output)

            # Parse result lines.
            found_any = False
            for line in output.splitlines():
                m = RESULT_RE.match(line)
                if m:
                    found_any = True
                    all_results.append((
                        rung,
                        m.group("name").strip(),
                        m.group("time"),
                        m.group("value"),
                        m.group("unit"),
                        m.group("pct"),
                    ))

            if not found_any:
                all_results.append((rung, "(no parsed results)", "", "", "", ""))

        except subprocess.TimeoutExpired:
            print(f"  TIMEOUT after 300s")
            all_results.append((rung, None, None, None, None, "TIMEOUT"))
        except Exception as e:
            print(f"  ERROR: {e}")
            all_results.append((rung, None, None, None, None, str(e)))

    # Print consolidated Markdown table.
    print("\n\n## Consolidated Results\n")
    print("| Rung | Kernel | Time (ms) | Throughput | Unit | % Peak |")
    print("|------|--------|-----------|------------|------|--------|")
    for entry in all_results:
        rung, name, time_ms, value, unit, pct = entry
        if name is None:
            print(f"| {rung} | — | — | — | — | {pct} |")
        else:
            print(f"| {rung} | {name} | {time_ms} | {value} | {unit or ''} | {pct} |")

    print("\n(Copy the table above into the README.md results section.)\n")


if __name__ == "__main__":
    main()
