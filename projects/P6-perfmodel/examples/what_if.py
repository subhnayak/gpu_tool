#!/usr/bin/env python3
"""
what_if.py — Architect-Style What-If Analysis
===============================================

"What happens to this kernel if we double L2, double bandwidth,
add 20% more SMs, or halve the memory latency?"

This is LITERALLY what the job means by "empower GPU architects to
understand application performance today and model industry-leading
performance for tomorrow."

The output is a readable report an architect would want to see.
"""

import sys
import os
import copy
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gpumodel.machine import MachineModel, ampere_a100
from gpumodel.kernel import KernelDescriptor
from gpumodel.model import predict


def what_if_report(machine: MachineModel, kernel: KernelDescriptor,
                   modifications: list) -> str:
    """
    Run a series of what-if modifications and produce a comparative report.

    Each modification is a dict:
      {"name": "Double bandwidth", "changes": {"memory_clock_ghz": machine.memory_clock_ghz * 2}}
    """
    # Baseline prediction
    baseline = predict(machine, kernel)
    baseline_time_us = baseline.predicted_time * 1e6

    lines = [
        "╔══════════════════════════════════════════════════════════════════╗",
        f"║  WHAT-IF ANALYSIS: {kernel.name} on {machine.name}",
        "╠══════════════════════════════════════════════════════════════════╣",
        f"║  Baseline: {baseline_time_us:.1f} µs ({baseline.dominant_term}-limited)",
        "║",
        f"║  {'Scenario':<35} {'Time':>8} {'Speedup':>8} {'Bottleneck':>12}",
        f"║  {'-'*65}",
    ]

    for mod in modifications:
        # Create modified machine
        m = copy.deepcopy(machine)
        for attr, value in mod["changes"].items():
            setattr(m, attr, value)

        result = predict(m, kernel)
        time_us = result.predicted_time * 1e6
        speedup = baseline_time_us / time_us if time_us > 0 else float('inf')

        lines.append(
            f"║  {mod['name']:<35} {time_us:>6.1f}µs {speedup:>6.2f}x  {result.dominant_term:>12}"
        )

    lines.extend([
        "║",
        "╠══════════════════════════════════════════════════════════════════╣",
        "║  INTERPRETATION GUIDE:",
        "║  - Speedup ≈ 1.0 → This change doesn't help (wrong bottleneck)",
        "║  - Speedup ≈ 2.0 → Linear scaling (this resource was the limiter)",
        "║  - Speedup < 2.0 with 2x resource → Partial benefit (other limits emerge)",
        "║  - Bottleneck shift → Adding resource A revealed bottleneck B",
        "╚══════════════════════════════════════════════════════════════════╝",
    ])

    return "\n".join(lines)


def main():
    machine = ampere_a100()
    N = 1 << 20  # 1M elements

    # ─── Analysis 1: Vector Add (memory-bound) ────────────────────────
    vector_add = KernelDescriptor(
        name="vector_add (1M)",
        grid_x=(N + 255) // 256, block_x=256,
        registers_per_thread=16,
        bytes_read_unique=N * 4 * 2,
        bytes_written_unique=N * 4,
        flops=N,
    )

    modifications = [
        {"name": "2x memory bandwidth",
         "changes": {"memory_clock_ghz": machine.memory_clock_ghz * 2}},
        {"name": "2x memory bus width",
         "changes": {"memory_bus_width_bits": machine.memory_bus_width_bits * 2}},
        {"name": "+20% SMs",
         "changes": {"sm_count": int(machine.sm_count * 1.2)}},
        {"name": "+50% clock speed",
         "changes": {"clock_ghz": machine.clock_ghz * 1.5}},
        {"name": "2x FP32 cores/SM",
         "changes": {"fp32_cores_per_sm": machine.fp32_cores_per_sm * 2}},
        {"name": "Half memory latency",
         "changes": {}},  # We'd need to modify MEMORY_LATENCY_NS in model.py
        {"name": "2x L2 cache (no direct BW effect)",
         "changes": {"l2_size_kb": machine.l2_size_kb * 2}},
        {"name": "Everything 2x (BW + compute)",
         "changes": {
             "memory_clock_ghz": machine.memory_clock_ghz * 2,
             "fp32_cores_per_sm": machine.fp32_cores_per_sm * 2,
         }},
    ]

    print(what_if_report(machine, vector_add, modifications))
    print()

    # ─── Analysis 2: SGEMM (compute-bound) ────────────────────────────
    M_gemm = 1024
    sgemm = KernelDescriptor(
        name="sgemm (1024x1024)",
        grid_x=M_gemm // 32, grid_y=M_gemm // 32,
        block_x=32, block_y=32,
        registers_per_thread=64,
        shared_memory_per_block=2 * 32 * 32 * 4,
        bytes_read_unique=2 * M_gemm * M_gemm * 4,
        bytes_written_unique=M_gemm * M_gemm * 4,
        bytes_read_total=2 * M_gemm * M_gemm * 4 * (M_gemm // 32),
        flops=2 * M_gemm ** 3,
    )

    print(what_if_report(machine, sgemm, modifications))
    print()

    # ─── Key Insight ──────────────────────────────────────────────────
    print("=" * 66)
    print("KEY INSIGHT FOR ARCHITECTS:")
    print("=" * 66)
    print("""
  For MEMORY-BOUND kernels (vector_add, transpose):
    → More bandwidth helps linearly
    → More compute cores DON'T help (already idle)
    → More SMs help only if they increase concurrent memory requests

  For COMPUTE-BOUND kernels (SGEMM):
    → More compute helps linearly
    → More bandwidth DOESN'T help (already idle)
    → More SMs help directly (more cores)

  For LATENCY-BOUND kernels (histogram with atomics):
    → Neither more bandwidth nor more compute helps much
    → Need algorithmic change (privatization, warp-level aggregation)

  THIS IS WHY THE ROOFLINE MODEL IS SO POWERFUL:
    It tells you which resource to invest in.
""")


if __name__ == "__main__":
    main()
