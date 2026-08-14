"""
roofline.py — Roofline Model Analysis
=======================================

The roofline model is THE foundational performance model for GPUs (and CPUs).

INTERVIEW: "Explain the roofline model."
- X-axis: Arithmetic Intensity (FLOP/byte)
- Y-axis: Achievable performance (GFLOPS)
- Two ceilings:
    1. Memory bandwidth ceiling: perf = AI * bandwidth (diagonal line)
    2. Compute ceiling: perf = peak_flops (horizontal line)
- A kernel's performance is bounded by min(compute_ceiling, bandwidth_ceiling)
- The intersection is the "ridge point"

The roofline tells you the THEORETICAL MAXIMUM performance for a kernel
with a given arithmetic intensity. If your kernel is far below the
roofline, there's optimization opportunity. If it's near the line,
you've hit the hardware limit for your algorithm.
"""

from .machine import MachineModel
from .kernel import KernelDescriptor
import math


def classify_kernel(machine: MachineModel, kernel: KernelDescriptor) -> str:
    """
    Classify a kernel as memory-bound or compute-bound.

    INTERVIEW: "How do you know if a kernel is memory-bound or compute-bound?"
    Compare its arithmetic intensity to the ridge point:
      AI < ridge → memory-bound (add more compute for free)
      AI > ridge → compute-bound (add more memory accesses for free)
      AI ≈ ridge → balanced (rare, optimal)
    """
    ai = kernel.arithmetic_intensity
    ridge = machine.ridge_point

    if ai < ridge * 0.8:
        return "memory-bound"
    elif ai > ridge * 1.2:
        return "compute-bound"
    else:
        return "balanced"


def predicted_gflops(machine: MachineModel, kernel: KernelDescriptor) -> float:
    """
    Predict performance in GFLOPS using the roofline model.

    perf = min(peak_flops, AI * peak_bandwidth)
    """
    ai = kernel.arithmetic_intensity
    mem_ceiling = ai * machine.achievable_bandwidth_gb_s
    compute_ceiling = machine.achievable_gflops
    return min(mem_ceiling, compute_ceiling)


def predicted_time_s(machine: MachineModel, kernel: KernelDescriptor) -> float:
    """
    Predict kernel execution time from the roofline model.

    time = flops / predicted_gflops  (in seconds, converting GFLOPS properly)
    """
    gflops = predicted_gflops(machine, kernel)
    if gflops <= 0 or kernel.flops <= 0:
        return 0.0
    return kernel.flops / (gflops * 1e9)


def roofline_analysis(machine: MachineModel, kernel: KernelDescriptor) -> str:
    """Full roofline analysis with text output."""
    ai = kernel.arithmetic_intensity
    classification = classify_kernel(machine, kernel)
    perf = predicted_gflops(machine, kernel)
    time = predicted_time_s(machine, kernel)

    lines = [
        f"=== Roofline Analysis: {kernel.name} on {machine.name} ===",
        f"  Arithmetic Intensity: {ai:.2f} FLOP/byte",
        f"  Ridge Point: {machine.ridge_point:.2f} FLOP/byte",
        f"  Classification: {classification.upper()}",
        f"  Memory ceiling: {ai * machine.achievable_bandwidth_gb_s:.1f} GFLOPS",
        f"  Compute ceiling: {machine.achievable_gflops:.1f} GFLOPS",
        f"  Predicted perf: {perf:.1f} GFLOPS",
    ]
    if time > 0:
        if time < 1e-3:
            lines.append(f"  Predicted time: {time*1e6:.1f} µs")
        else:
            lines.append(f"  Predicted time: {time*1e3:.3f} ms")
    return "\n".join(lines)


def ascii_roofline(machine: MachineModel, kernels: list, width: int = 60, height: int = 20) -> str:
    """
    Text-mode ASCII roofline plot.

    This is a log-log plot:
      X-axis: log2(Arithmetic Intensity)
      Y-axis: log2(Performance in GFLOPS)

    Kernels are plotted as labeled points.
    """
    if not kernels:
        return "No kernels to plot."

    # Compute ranges
    ais = [k.arithmetic_intensity for k in kernels if k.arithmetic_intensity > 0]
    if not ais:
        return "No valid kernels to plot."

    min_ai = min(min(ais) * 0.5, 0.1)
    max_ai = max(max(ais) * 2, machine.ridge_point * 4)

    peak_gflops = machine.achievable_gflops
    peak_bw = machine.achievable_bandwidth_gb_s
    min_perf = min(min_ai * peak_bw * 0.5, 1.0)
    max_perf = peak_gflops * 1.5

    def log_scale(val, lo, hi, steps):
        if val <= 0 or lo <= 0:
            return 0
        log_val = math.log2(max(val, lo))
        log_lo = math.log2(lo)
        log_hi = math.log2(hi)
        if log_hi == log_lo:
            return 0
        return int((log_val - log_lo) / (log_hi - log_lo) * (steps - 1))

    # Build the canvas
    canvas = [[' ' for _ in range(width)] for _ in range(height)]

    # Draw the roofline (memory ceiling and compute ceiling)
    for col in range(width):
        # AI at this column
        log_lo = math.log2(min_ai)
        log_hi = math.log2(max_ai)
        log_ai = log_lo + (col / (width - 1)) * (log_hi - log_lo)
        ai = 2 ** log_ai

        # Roofline performance at this AI
        perf = min(ai * peak_bw, peak_gflops)
        row = height - 1 - log_scale(perf, min_perf, max_perf, height)
        row = max(0, min(height - 1, row))
        canvas[row][col] = '-' if perf >= peak_gflops * 0.99 else '/'

    # Plot kernel points
    markers = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    legend = []
    for i, k in enumerate(kernels):
        ai = k.arithmetic_intensity
        if ai <= 0:
            continue
        perf = min(ai * peak_bw, peak_gflops)
        col = log_scale(ai, min_ai, max_ai, width)
        row = height - 1 - log_scale(perf, min_perf, max_perf, height)
        col = max(0, min(width - 1, col))
        row = max(0, min(height - 1, row))
        marker = markers[i % len(markers)]
        canvas[row][col] = marker
        legend.append(f"  {marker} = {k.name} (AI={ai:.2f})")

    # Render
    lines = ["  GFLOPS"]
    for row in canvas:
        lines.append("  |" + "".join(row))
    lines.append("  +" + "-" * width)
    lines.append("   " + " " * (width // 2 - 10) + "Arithmetic Intensity (FLOP/byte)")
    lines.append("")
    lines.append("  Legend:")
    lines.extend(legend)
    lines.append(f"  Ridge point: {machine.ridge_point:.2f} FLOP/byte")
    return "\n".join(lines)


def matplotlib_roofline(machine: MachineModel, kernels: list, output_file: str = None):
    """
    Matplotlib roofline plot. Only called if matplotlib is available.
    Guarded by import check — fails gracefully without matplotlib.
    """
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib not available — use ascii_roofline() instead")
        return

    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    ax.set_xscale('log', base=2)
    ax.set_yscale('log', base=2)

    # Draw roofline
    ais = np.logspace(-2, 4, 1000, base=2)
    perfs = np.minimum(ais * machine.achievable_bandwidth_gb_s, machine.achievable_gflops)
    ax.plot(ais, perfs, 'b-', linewidth=2, label='Roofline')

    # Plot kernels
    for k in kernels:
        ai = k.arithmetic_intensity
        if ai > 0:
            ax.plot(ai, min(ai * machine.achievable_bandwidth_gb_s, machine.achievable_gflops),
                    'ro', markersize=10)
            ax.annotate(k.name, (ai, min(ai * machine.achievable_bandwidth_gb_s,
                        machine.achievable_gflops)), textcoords="offset points",
                        xytext=(5, 5))

    ax.axvline(x=machine.ridge_point, color='gray', linestyle='--', alpha=0.5,
               label=f'Ridge={machine.ridge_point:.1f}')
    ax.set_xlabel('Arithmetic Intensity (FLOP/byte)')
    ax.set_ylabel('Performance (GFLOPS)')
    ax.set_title(f'Roofline Model — {machine.name}')
    ax.legend()
    ax.grid(True, alpha=0.3)

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Saved roofline plot to {output_file}")
    else:
        plt.show()
