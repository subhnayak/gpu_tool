"""
validate.py — Compare Predicted vs Measured Kernel Performance
================================================================

Loads measured results (JSON or CSV), runs the model, and compares.

EXPECTED INPUT FORMAT (from P3's run_all.py):
  JSON: [{"name": "vector_add", "time_ms": 0.123, "bytes": ..., "flops": ...}, ...]
  CSV:  name,time_ms,bytes_read,bytes_written,flops,block_size,regs_per_thread,shared_mem

The model is intentionally simple. Understanding WHERE and WHY it fails
is the learning objective. The validate output highlights:
  - Error percentage per kernel
  - Which kernels the model gets right (and why)
  - Which kernels the model gets wrong (and why)
"""

import json
import csv
import os
from .machine import MachineModel
from .kernel import KernelDescriptor
from .model import predict, explain


def load_measured_json(filepath: str) -> list:
    """Load measured results from a JSON file."""
    with open(filepath, 'r') as f:
        return json.load(f)


def load_measured_csv(filepath: str) -> list:
    """Load measured results from a CSV file."""
    results = []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            results.append({
                'name': row.get('name', 'unknown'),
                'time_ms': float(row.get('time_ms', 0)),
                'bytes_read': int(row.get('bytes_read', 0)),
                'bytes_written': int(row.get('bytes_written', 0)),
                'flops': int(row.get('flops', 0)),
                'block_size': int(row.get('block_size', 256)),
                'regs_per_thread': int(row.get('regs_per_thread', 32)),
                'shared_mem': int(row.get('shared_mem', 0)),
            })
    return results


def make_kernel_from_measured(m: dict, n_elements: int = 0) -> KernelDescriptor:
    """Convert a measured result dict to a KernelDescriptor."""
    block_size = m.get('block_size', 256)
    total_threads = m.get('total_threads', n_elements if n_elements else 1 << 20)
    grid_size = max(1, (total_threads + block_size - 1) // block_size)

    return KernelDescriptor(
        name=m.get('name', 'unknown'),
        grid_x=grid_size,
        block_x=block_size,
        registers_per_thread=m.get('regs_per_thread', 32),
        shared_memory_per_block=m.get('shared_mem', 0),
        bytes_read_unique=m.get('bytes_read', 0),
        bytes_written_unique=m.get('bytes_written', 0),
        flops=m.get('flops', 0),
    )


def validate(machine: MachineModel, measured_results: list,
             n_elements: int = 1 << 20, verbose: bool = True) -> str:
    """
    Compare predictions against measurements.

    Returns a formatted table with error analysis.
    """
    lines = [
        f"{'Kernel':<20} {'Measured':>10} {'Predicted':>10} {'Error':>8} "
        f"{'Model Says':>12} {'Actual':>12}",
        "-" * 80,
    ]

    errors = []

    for m in measured_results:
        kernel = make_kernel_from_measured(m, n_elements)
        result = predict(machine, kernel)

        measured_time_ms = m.get('time_ms', 0)
        predicted_time_ms = result.predicted_time * 1000  # s → ms

        if measured_time_ms > 0:
            error_pct = (predicted_time_ms - measured_time_ms) / measured_time_ms * 100
        else:
            error_pct = float('nan')

        errors.append(abs(error_pct) if not (error_pct != error_pct) else 0)

        # Determine actual classification from measurement
        # If measured time ≈ bytes / bandwidth → memory-bound
        # If measured time ≈ flops / peak_flops → compute-bound
        ideal_mem_time = kernel.total_bytes / (machine.achievable_bandwidth_gb_s * 1e9) * 1000
        ideal_comp_time = kernel.flops / (machine.achievable_gflops * 1e9) * 1000 if kernel.flops > 0 else 0

        if ideal_mem_time > 0 and measured_time_ms > 0:
            bw_util = ideal_mem_time / measured_time_ms
            if bw_util > 0.5:
                actual_class = "mem-bound"
            elif ideal_comp_time > 0 and ideal_comp_time / measured_time_ms > 0.5:
                actual_class = "comp-bound"
            else:
                actual_class = "latency?"
        else:
            actual_class = "unknown"

        lines.append(
            f"{kernel.name:<20} {measured_time_ms:>8.3f}ms {predicted_time_ms:>8.3f}ms "
            f"{error_pct:>+7.1f}% {result.dominant_term:>12} {actual_class:>12}"
        )

        if verbose:
            lines.append(f"    AI={kernel.arithmetic_intensity:.2f} "
                         f"occ={result.occupancy.occupancy*100:.0f}% "
                         f"({result.occupancy.binding_constraint})")

    lines.append("-" * 80)
    if errors:
        avg_err = sum(errors) / len(errors)
        max_err = max(errors)
        lines.append(f"Average |error|: {avg_err:.1f}%  Max |error|: {max_err:.1f}%")
        lines.append("")
        lines.append("ANALYSIS:")
        if avg_err < 30:
            lines.append("  Model is reasonably accurate (< 30% average error).")
            lines.append("  This is typical for a simple analytical model on memory-bound kernels.")
        else:
            lines.append("  Model has significant errors (> 30% average).")
            lines.append("  Likely causes: cache effects, latency hiding, or workload-specific")
            lines.append("  behavior not captured by the simple bandwidth/compute model.")
        lines.append("")
        lines.append("WHERE THE MODEL FAILS AND WHY:")
        lines.append("  - Kernels with high cache reuse (reduction, SGEMM): model overpredicts")
        lines.append("    because it uses DRAM bandwidth, but data may be served from L2/L1")
        lines.append("  - Kernels with low occupancy: model may undershoot because latency")
        lines.append("    hiding is insufficient but our latency model is approximate")
        lines.append("  - Kernels with atomic contention (histogram): model ignores serialization")

    return "\n".join(lines)
