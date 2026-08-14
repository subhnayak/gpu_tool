"""
model.py — GPU Kernel Performance Predictor
=============================================

Combines the roofline model, occupancy analysis, and Little's Law
to predict kernel execution time.

MODEL EQUATION:
  predicted_time = max(memory_time, compute_time) * overlap_factor + launch_overhead

Where:
  memory_time  = total_bytes / (peak_bandwidth * bw_efficiency)
  compute_time = flops / (peak_flops * compute_efficiency)
  overlap_factor = accounts for partial overlap of memory and compute
                   (GPUs overlap them, so max() overpredicts slightly)

HONEST LIMITATIONS:
  - Simple analytical models are typically within ~20-30% for well-behaved
    memory-bound kernels
  - Much worse (50%+) for latency-bound kernels, kernels with complex
    control flow, or kernels limited by instruction cache/scheduling
  - UNDERSTANDING WHY IT IS WRONG is the actual learning objective

INTERVIEW: "How would you predict the performance of a GPU kernel?"
Walk through this model, state your assumptions, and discuss limitations.
"""

from dataclasses import dataclass
from .machine import MachineModel
from .kernel import KernelDescriptor
from .occupancy import calculate_occupancy, OccupancyResult
from .roofline import classify_kernel, predicted_gflops
import math


@dataclass
class PredictionResult:
    """Detailed prediction with full reasoning chain."""
    kernel_name: str
    machine_name: str

    # Times in seconds
    memory_time: float
    compute_time: float
    latency_bound_time: float
    launch_overhead: float
    predicted_time: float

    # Analysis
    dominant_term: str      # "memory", "compute", or "latency"
    classification: str     # from roofline
    occupancy: OccupancyResult
    arithmetic_intensity: float

    # Components for explanation
    bw_efficiency: float
    compute_efficiency: float


# ═══════════════════════════════════════════════════════════════════════
# Launch overhead — approximate kernel launch cost
# ═══════════════════════════════════════════════════════════════════════
# INTERVIEW: "What is kernel launch overhead?"
# Every kernel launch has a fixed cost (~5-20 µs on modern GPUs) for:
#   - Pushing the launch packet to the GPU command queue
#   - GPU hardware reading the launch configuration
#   - First warp scheduling
# This is why launching many tiny kernels is inefficient.
LAUNCH_OVERHEAD_S = 5e-6  # 5 microseconds (conservative estimate)

# Memory latency in nanoseconds (DRAM access)
# INTERVIEW: "What is GPU memory latency?"
# ~400-600 ns for GDDR6/HBM. Much higher than L1 (~30 cycles ≈ 20ns)
# or L2 (~200 cycles ≈ 130ns). This is why latency hiding via
# massive parallelism is crucial.
MEMORY_LATENCY_NS = 500


def predict(machine: MachineModel, kernel: KernelDescriptor) -> PredictionResult:
    """
    Predict kernel execution time.

    The model:
      1. Memory time = bytes / (achievable_bandwidth)
      2. Compute time = flops / (achievable_flops)
      3. Latency-bound estimate via Little's Law
      4. Predicted time = max(memory, compute, latency) + launch_overhead

    The KEY INSIGHT is step 3: even if you have enough bandwidth and FLOPs,
    you need enough PARALLELISM to hide the memory latency. This is what
    occupancy helps with.
    """

    occ = calculate_occupancy(machine, kernel)
    classification = classify_kernel(machine, kernel)
    ai = kernel.arithmetic_intensity

    # ─── Memory Time ──────────────────────────────────────────────────
    # How long to move all the data, assuming we use all available bandwidth.
    #
    # Adjust efficiency based on occupancy — lower occupancy means fewer
    # concurrent memory requests, which reduces achieved bandwidth.
    # This is a simplification; real bandwidth depends on access pattern.
    bw_occ_factor = min(1.0, 0.3 + 0.7 * occ.occupancy)
    bw_efficiency = machine.achievable_bandwidth_fraction * bw_occ_factor
    effective_bw = machine.peak_bandwidth_gb_s * bw_efficiency  # GB/s

    total_bytes = kernel.total_bytes
    memory_time = total_bytes / (effective_bw * 1e9) if effective_bw > 0 else 0

    # ─── Compute Time ─────────────────────────────────────────────────
    # How long to execute all the FLOPs.
    comp_occ_factor = min(1.0, 0.5 + 0.5 * occ.occupancy)
    compute_efficiency = machine.achievable_flops_fraction * comp_occ_factor
    effective_flops = machine.peak_fp32_gflops * compute_efficiency  # GFLOPS

    compute_time = kernel.flops / (effective_flops * 1e9) if effective_flops > 0 else 0

    # ─── Latency-Bound Estimate (Little's Law) ────────────────────────
    # Little's Law: to saturate bandwidth B with latency L, you need
    # N = B * L concurrent requests, where each request is of size S.
    #
    # N_needed = (bandwidth * latency) / request_size
    #          = (effective_bw_GB/s * latency_ns * 1e-9) / 128 bytes
    #            (128 bytes = cache line size)
    #
    # If the kernel can't provide enough concurrent requests (limited by
    # occupancy), it's latency-bound.
    #
    # INTERVIEW: "Explain Little's Law for GPU memory."
    # To hide memory latency L, you need N = B*L/S outstanding requests.
    # Each warp can have ~1 outstanding request, so you need at least N warps.
    # If occupancy gives you fewer warps, bandwidth drops proportionally.
    cache_line_bytes = 128
    latency_s = MEMORY_LATENCY_NS * 1e-9
    needed_concurrent_requests = (effective_bw * 1e9 * latency_s) / cache_line_bytes

    # Available concurrent requests ≈ total warps across all SMs
    # (each warp can have one outstanding memory request)
    total_warps = occ.warps_per_sm * machine.sm_count
    available_requests = total_warps

    latency_bound_time = 0.0
    if available_requests < needed_concurrent_requests and available_requests > 0:
        # We can't fully hide latency — effective bandwidth is reduced
        actual_fraction = available_requests / needed_concurrent_requests
        reduced_bw = effective_bw * actual_fraction
        latency_bound_time = total_bytes / (reduced_bw * 1e9) if reduced_bw > 0 else 0

    # ─── Combined Prediction ──────────────────────────────────────────
    # The GPU overlaps memory and compute, so max() is the right model.
    # We take max of all three estimates.
    base_time = max(memory_time, compute_time, latency_bound_time)
    predicted_time = base_time + LAUNCH_OVERHEAD_S

    # Determine dominant term
    if latency_bound_time >= memory_time and latency_bound_time >= compute_time:
        dominant = "latency"
    elif memory_time >= compute_time:
        dominant = "memory"
    else:
        dominant = "compute"

    return PredictionResult(
        kernel_name=kernel.name,
        machine_name=machine.name,
        memory_time=memory_time,
        compute_time=compute_time,
        latency_bound_time=latency_bound_time,
        launch_overhead=LAUNCH_OVERHEAD_S,
        predicted_time=predicted_time,
        dominant_term=dominant,
        classification=classification,
        occupancy=occ,
        arithmetic_intensity=ai,
        bw_efficiency=bw_efficiency,
        compute_efficiency=compute_efficiency,
    )


def explain(result: PredictionResult) -> str:
    """
    Print the full reasoning chain — WHY the model predicted what it did.

    THIS IS THE MOST VALUABLE PART. In an interview, you'd walk through
    this exact reasoning:
    1. Here's the kernel's resource usage → occupancy
    2. Here's the arithmetic intensity → roofline classification
    3. Here's the memory time, compute time, latency estimate
    4. Here's which one dominates and WHY
    5. Here's the final prediction and caveats
    """
    r = result
    time_us = r.predicted_time * 1e6

    lines = [
        f"╔══════════════════════════════════════════════════════════════╗",
        f"║  Performance Prediction: {r.kernel_name}",
        f"║  Machine: {r.machine_name}",
        f"╠══════════════════════════════════════════════════════════════╣",
        f"║",
        f"║  Step 1: OCCUPANCY",
        f"║  {r.occupancy.summary()}",
        f"║",
        f"║  Step 2: ROOFLINE CLASSIFICATION",
        f"║  Arithmetic Intensity: {r.arithmetic_intensity:.2f} FLOP/byte",
        f"║  Classification: {r.classification.upper()}",
        f"║",
        f"║  Step 3: TIME ESTIMATES",
        f"║  Memory time:  {r.memory_time*1e6:.1f} µs  (BW efficiency: {r.bw_efficiency*100:.0f}%)",
        f"║  Compute time: {r.compute_time*1e6:.1f} µs  (Compute eff:   {r.compute_efficiency*100:.0f}%)",
        f"║  Latency time: {r.latency_bound_time*1e6:.1f} µs  (Little's Law bound)",
        f"║  Launch overhead: {r.launch_overhead*1e6:.0f} µs",
        f"║",
        f"║  Step 4: DOMINANT TERM",
        f"║  The kernel is {r.dominant_term.upper()}-limited.",
    ]

    if r.dominant_term == "memory":
        lines.append(f"║  → Memory time dominates. To speed up: reduce data movement,")
        lines.append(f"║    improve coalescing, use shared memory, or increase cache hit rate.")
    elif r.dominant_term == "compute":
        lines.append(f"║  → Compute time dominates. To speed up: reduce instruction count,")
        lines.append(f"║    use faster instructions (FMA, tensor cores), or reduce precision.")
    else:
        lines.append(f"║  → Latency dominates (not enough parallelism to hide memory latency).")
        lines.append(f"║    To speed up: increase occupancy (reduce regs/shared), increase block count.")

    lines.extend([
        f"║",
        f"║  Step 5: PREDICTION",
        f"║  ┌─────────────────────────────────────┐",
        f"║  │ Predicted time: {time_us:.1f} µs             │",
        f"║  └─────────────────────────────────────┘",
        f"║",
        f"║  CAVEATS:",
        f"║  - This is a SIMPLIFIED analytical model",
        f"║  - Typical accuracy: ±20-30% for memory-bound kernels",
        f"║  - Worse for latency-bound, control-flow-heavy, or cache-sensitive kernels",
        f"║  - Does not model: L1/L2 cache effects, bank conflicts, warp divergence,",
        f"║    instruction scheduling, PCIe transfer, or multi-kernel overlap",
        f"╚══════════════════════════════════════════════════════════════╝",
    ])
    return "\n".join(lines)
