# Exercises — Extending the CUDA Ladder

These 12 graded exercises build on the ladder rungs.  Each one pushes you to
explore a concept deeper.  For every exercise, **measure** and **explain** what
you see.

---

## E1. Peak Copy Bandwidth

Write a bare `float4` copy kernel (no arithmetic) and tune block size to find
your GPU's actual peak device-to-device bandwidth.

**Hint:** Start with 256 threads/block, try 128 and 512.  Use `ncu` to confirm
you're limited by DRAM throughput (`dram__bytes.sum.per_second`).

**Expected:** You should reach 85-95% of the theoretical peak from `deviceQuery`.

---

## E2. L2 vs DRAM Bandwidth

Modify the vector-add kernel to work on varying data sizes: 32 KB, 128 KB,
512 KB, 2 MB, 8 MB, 32 MB.  Plot bandwidth vs working-set size.

**Hint:** Small working sets fit in L2 cache → much higher effective bandwidth.
The transition point is where the working set exceeds L2 size.

**Expected:** A step-function drop in bandwidth at the L2 boundary.

---

## E3. Segmented Reduction

Implement a segmented reduction: given an array and segment-head flags,
reduce each segment independently.

**Hint:** Use a modified scan-based approach.  Reset the accumulator at
segment boundaries.

**Expected:** Correctness first.  Performance should be near the flat reduction
for long segments.

---

## E4. Top-K Selection

Given N floats, find the top K (e.g., K=1024).  Implement both:
(a) Sort the full array, take last K.
(b) A heap-based or bitonic-sort-based partial selection.

**Hint:** For large N, a block-level partial sort + merge is faster than full sort.

**Expected:** The partial approach wins for K ≪ N.

---

## E5. 2D Stencil with Halo Loading

Implement a 5-point 2D stencil (Jacobi iteration).  First, a naive version.
Then optimize with shared memory and halo cells.

**Hint:** Each block loads a (TILE+2)×(TILE+2) region including the 1-cell halo.
Use `__syncthreads()` between loading and computing.

**Expected:** Shared-memory version should be ~2-3× faster due to reduced
redundant global loads (each interior cell is loaded once instead of 5 times).

---

## E6. __ldg and Vectorized Loads

Take the coalescing rung's coalesced copy and compare:
(a) Scalar `float` loads.
(b) `__ldg()` (read-only cache path).
(c) `float4` vectorized loads.
(d) `float4` + `__ldg()`.

**Hint:** On Kepler+, `__ldg` uses the read-only texture cache.  On Maxwell+
with `__restrict__`, the compiler does this automatically, so `__ldg` may not
help further.

**Expected:** `float4` ≥ scalar.  `__ldg` may or may not help depending on arch.

---

## E7. Kernel Fusion

Take the vector-add kernel and a scale kernel (`C[i] = alpha * A[i]`).
Benchmark them separately vs fused (`C[i] = alpha * A[i] + B[i]`).

**Hint:** Two separate kernels load A from DRAM twice; the fused kernel loads
it once.

**Expected:** Fused kernel approaches 1.5× the throughput of the two-kernel
version (3 loads+stores vs 4).

---

## E8. Warp-Level Matrix Ops (WMMA)

Use `nvcuda::wmma` to implement a small matrix multiply with Tensor Cores
(if your GPU supports them — compute ≥ 7.0).

**Hint:** Use `wmma::mma_sync` with 16×16×16 fragments.  Compare against your
SGEMM register-tiled kernel.

**Expected:** Tensor Cores should achieve dramatically higher GFLOP/s for
FP16 inputs.

---

## E9. Atomic Throughput vs Contention

Write a micro-benchmark: N threads all atomicAdd to M distinct locations.
Vary M from 1 to N and plot throughput.

**Hint:** M=1 → maximum contention → serialised.  M=N → no contention →
peak atomics throughput.

**Expected:** A curve showing throughput increasing from M=1 to a plateau
around M = (number of SMs × ~32).

---

## E10. Launch Overhead Crossover

Benchmark a trivial kernel (just return) for various grid sizes (1 block, 10,
100, 1000).  Find the minimum kernel duration where GPU execution dominates
launch overhead.

**Hint:** Use CUDA events.  The CPU-side `cudaLaunchKernel` has ~5-10 µs
overhead.

**Expected:** Kernels under ~10 µs are launch-overhead-dominated.  Graph
launch reduces this.

---

## E11. Cooperative Groups Reduction

Rewrite the reduction kernel using Cooperative Groups (`cg::thread_block`,
`cg::tiled_partition<32>`) instead of raw `__shfl_down_sync`.

**Hint:** `auto tile = cg::tiled_partition<32>(cg::this_thread_block());`
then `tile.shfl_down(val, offset)`.

**Expected:** Same performance as manual shuffle, but cleaner and more portable.

---

## E12. Multi-GPU Peer Access

If you have multiple GPUs (or can simulate with MPS), implement a vector add
that splits work across two GPUs with peer-to-peer memory access.

**Hint:** `cudaDeviceCanAccessPeer`, `cudaDeviceEnablePeerAccess`,
`cudaMemcpyPeerAsync`.

**Expected:** Near-linear speedup for large enough data, minus NVLink/PCIe
transfer overhead.

---

## Submission Checklist

For each exercise:
- [ ] Code compiles and runs correctly
- [ ] Verified against CPU reference
- [ ] Profiled with `ncu` or `nsys`
- [ ] Results documented with explanation
- [ ] Connected to at least one interview question
