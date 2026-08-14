#!/usr/bin/env python3
"""
predict_p3.py — Worked Predictions for Each P3 Kernel Ladder Rung
===================================================================

This is a set of WORKED INTERVIEW ANSWERS. For each classic GPU kernel,
we derive the arithmetic intensity by hand, predict performance, and
explain the reasoning.

INTERVIEW: "Walk me through the arithmetic intensity of vector add."
(The answer is below, with the math shown.)
"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gpumodel.machine import MachineModel, ampere_a100, ada_rtx4090
from gpumodel.kernel import KernelDescriptor
from gpumodel.model import predict, explain
from gpumodel.roofline import roofline_analysis, ascii_roofline

# Use A100 as our reference machine
machine = ampere_a100()
print(machine.summary())
print()

N = 1 << 20  # 1M elements (4 MB for float32)

# ═══════════════════════════════════════════════════════════════════════
# RUNG 1: VECTOR ADD  —  c[i] = a[i] + b[i]
# ═══════════════════════════════════════════════════════════════════════
#
# ARITHMETIC INTENSITY DERIVATION:
#   Reads: 2 arrays × N × 4 bytes = 8N bytes
#   Writes: 1 array × N × 4 bytes = 4N bytes
#   Total bytes: 12N
#   FLOPs: N additions = N
#   AI = N / 12N = 1/12 ≈ 0.083 FLOP/byte
#
# This is HEAVILY memory-bound (AI << ridge point).
# The kernel's performance is entirely determined by memory bandwidth.
#
# INTERVIEW: "Why is vector add memory-bound?"
# Because it does only 1 FLOP per 12 bytes of data movement.
# The GPU has >10 TFLOPS of compute but only ~2 TB/s bandwidth,
# so the ridge point is ~5 FLOP/byte. At AI=0.08, we're far below.

vector_add = KernelDescriptor(
    name="vector_add",
    grid_x=(N + 255) // 256,
    block_x=256,
    registers_per_thread=16,
    bytes_read_unique=N * 4 * 2,    # 2 input arrays
    bytes_written_unique=N * 4,     # 1 output array
    flops=N,                         # N additions
)

print("=" * 60)
print("RUNG 1: VECTOR ADD")
print("=" * 60)
print(f"  Hand-derived AI = N / 12N = {1/12:.4f} FLOP/byte")
print(f"  Computed AI = {vector_add.arithmetic_intensity:.4f} FLOP/byte")
result = predict(machine, vector_add)
print(explain(result))
print()

# ═══════════════════════════════════════════════════════════════════════
# RUNG 2: MATRIX TRANSPOSE
# ═══════════════════════════════════════════════════════════════════════
#
# AI DERIVATION:
#   Reads: N×N × 4 bytes = 4N² bytes
#   Writes: N×N × 4 bytes = 4N² bytes
#   Total: 8N² bytes
#   FLOPs: 0 (just data movement)
#   AI = 0 / 8N² = 0 FLOP/byte
#
# Pure memory bandwidth test! No compute at all.
# The key optimization is SHARED MEMORY to coalesce writes:
#   - Naive: read coalesced, write strided (bad: ~10-20% BW utilization)
#   - Shared mem: read coalesced → shared mem → write coalesced (~80% BW)

M = 1024  # 1024x1024 matrix
transpose = KernelDescriptor(
    name="transpose",
    grid_x=M // 32, grid_y=M // 32,
    block_x=32, block_y=8,
    registers_per_thread=12,
    shared_memory_per_block=32 * 33 * 4,  # +1 column to avoid bank conflicts
    bytes_read_unique=M * M * 4,
    bytes_written_unique=M * M * 4,
    flops=0,  # no compute
)

print("=" * 60)
print("RUNG 2: MATRIX TRANSPOSE")
print("=" * 60)
print(f"  AI = 0 FLOP/byte (pure memory movement)")
result = predict(machine, transpose)
print(explain(result))
print()

# ═══════════════════════════════════════════════════════════════════════
# RUNG 3: REDUCTION (sum of array)
# ═══════════════════════════════════════════════════════════════════════
#
# AI DERIVATION:
#   Reads: N × 4 bytes from DRAM (each element read once)
#   Writes: ~1 float (the sum) — negligible
#   Total bytes: ~4N
#   FLOPs: N-1 additions ≈ N
#   AI = N / 4N = 0.25 FLOP/byte
#
# Still memory-bound, but higher AI than vector add.
# The trick: reduction has log2(N) STEPS but the total work
# and memory traffic are both O(N). The challenge is keeping
# the GPU busy as the number of active threads halves each step.

reduction = KernelDescriptor(
    name="reduction",
    grid_x=(N + 255) // 256,
    block_x=256,
    registers_per_thread=16,
    shared_memory_per_block=256 * 4,  # one float per thread
    bytes_read_unique=N * 4,
    bytes_written_unique=4,  # just the final sum
    flops=N,
    dependency_chain_cycles=20,  # log2(256) * ~2 cycles
)

print("=" * 60)
print("RUNG 3: REDUCTION")
print("=" * 60)
print(f"  AI = N / 4N = {1/4:.2f} FLOP/byte")
result = predict(machine, reduction)
print(explain(result))
print()

# ═══════════════════════════════════════════════════════════════════════
# RUNG 4: SGEMM (Matrix Multiply)  —  C = A × B
# ═══════════════════════════════════════════════════════════════════════
#
# AI DERIVATION (naive):
#   For N×N matrices:
#   Reads: 2 × N² × 4 bytes = 8N² (each matrix read once... ideally)
#   Writes: N² × 4 bytes = 4N²
#   FLOPs: 2N³ (N² output elements, each = dot product of N elements = N MADs = 2N FLOPs)
#   AI_naive = 2N³ / 12N² = N/6
#   For N=1024: AI = 1024/6 ≈ 170 FLOP/byte  ← VERY compute-bound
#
# BUT: naive implementation doesn't achieve this because each thread
# reads from DRAM multiple times. With tiling (shared memory):
#   Each tile loads T×T elements, computes T³ FLOPs
#   AI_tiled = 2T / (2 * 4) = T/4
#   For T=32: AI = 8 FLOP/byte  ← still compute-bound on most GPUs
#
# INTERVIEW: "Why is GEMM considered the gold standard for GPU compute?"
# Because its AI grows with matrix size, making it one of the few kernels
# that can be genuinely compute-bound and approach peak FLOPS.

M_gemm = 1024
sgemm = KernelDescriptor(
    name="sgemm",
    grid_x=M_gemm // 32, grid_y=M_gemm // 32,
    block_x=32, block_y=32,
    registers_per_thread=64,
    shared_memory_per_block=2 * 32 * 32 * 4,  # two tiles in shared mem
    bytes_read_unique=2 * M_gemm * M_gemm * 4,  # A and B
    bytes_written_unique=M_gemm * M_gemm * 4,   # C
    # With tiling, effective bytes are higher due to tile loads
    bytes_read_total=2 * M_gemm * M_gemm * 4 * (M_gemm // 32),  # each tile loaded M/T times
    flops=2 * M_gemm ** 3,  # 2N³ (multiply-add = 2 ops)
)

print("=" * 60)
print("RUNG 4: SGEMM")
print("=" * 60)
print(f"  Naive AI = 2N³/12N² = N/6 = {M_gemm/6:.0f} FLOP/byte")
print(f"  Computed AI (with total bytes) = {sgemm.arithmetic_intensity:.2f} FLOP/byte")
result = predict(machine, sgemm)
print(explain(result))
print()

# ═══════════════════════════════════════════════════════════════════════
# RUNG 5: HISTOGRAM
# ═══════════════════════════════════════════════════════════════════════
#
# AI DERIVATION:
#   Reads: N × 4 bytes (input data)
#   Writes: B × 4 bytes (B bins) — negligible if B << N
#   FLOPs: ~0 (just bin lookup + atomic increment)
#   AI ≈ 0 FLOP/byte
#
# Histogram is neither memory-bound nor compute-bound — it's
# LATENCY-BOUND due to atomic contention. Many threads try to
# atomically increment the same bin, serializing at the atomic unit.
#
# INTERVIEW: "Why is histogram hard to optimize on a GPU?"
# Atomic contention. Solutions: privatized histograms (one per block
# in shared memory), then merge. This reduces contention from
# all-threads-globally to all-threads-in-one-block.

histogram = KernelDescriptor(
    name="histogram",
    grid_x=(N + 255) // 256,
    block_x=256,
    registers_per_thread=16,
    shared_memory_per_block=256 * 4,  # privatized histogram
    bytes_read_unique=N * 4,
    bytes_written_unique=256 * 4,
    flops=N,  # approximate: address calc + increment
)

print("=" * 60)
print("RUNG 5: HISTOGRAM")
print("=" * 60)
print(f"  AI = {histogram.arithmetic_intensity:.4f} FLOP/byte")
print("  Note: histogram is limited by ATOMIC CONTENTION, not bandwidth or compute.")
print("  The model will likely be WRONG here — this is expected and instructive.")
result = predict(machine, histogram)
print(explain(result))
print()

# ═══════════════════════════════════════════════════════════════════════
# ROOFLINE PLOT — ALL KERNELS
# ═══════════════════════════════════════════════════════════════════════
all_kernels = [vector_add, transpose, reduction, sgemm, histogram]
print("=" * 60)
print("ROOFLINE PLOT")
print("=" * 60)
print(ascii_roofline(machine, all_kernels))
