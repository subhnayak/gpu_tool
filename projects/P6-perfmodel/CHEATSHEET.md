# GPU Performance Modeling Cheatsheet

Formulas on one page. Memorize these for interviews.

---

## Peak Performance

**Peak FP32 GFLOPS** = SM_count × cores_per_SM × clock_GHz × 2 (FMA)

**Peak Bandwidth (GB/s)** = bus_width_bits × mem_clock_GHz × 2 (DDR) / 8

**Example (A100):**
  108 SMs × 64 cores × 1.41 GHz × 2 = 19,468 GFLOPS
  5120 bits × 1.215 GHz × 2 / 8 = 1,555 GB/s

---

## Arithmetic Intensity (AI)

**AI = FLOPs / Bytes transferred**  (FLOP/byte)

| Kernel | FLOPs | Bytes | AI |
|--------|-------|-------|----|
| Vector add: c=a+b | N | 12N | 1/12 ≈ 0.08 |
| Dot product | 2N | 8N | 1/4 = 0.25 |
| SAXPY: y=ax+y | 2N | 12N | 1/6 ≈ 0.17 |
| Matrix transpose | 0 | 8N² | 0 |
| Reduction (sum) | N | 4N | 1/4 = 0.25 |
| SGEMM (naive) | 2N³ | 12N² | N/6 |
| SGEMM (tiled, T) | 2N³ | 2N³/T × 4 | T/4 |
| SpMV (CSR) | 2×nnz | (12×nnz + 4N) | ~0.17 |
| Stencil (3D, 7pt) | 8N³ | ~8N³ | ~1.0 |

---

## Roofline Model

**Predicted GFLOPS = min(AI × BW, Peak_GFLOPS)**

**Ridge Point = Peak_GFLOPS / Peak_BW**  (FLOP/byte)

- AI < Ridge → **memory-bound** (bandwidth ceiling)
- AI > Ridge → **compute-bound** (compute ceiling)

---

## Occupancy

**Occupancy = achieved_warps / max_warps_per_SM**

Four limits (take the minimum):

| Resource | Blocks per SM |
|----------|---------------|
| Registers | floor(reg_file / (regs_per_thread × threads_per_block)) |
| Shared Memory | floor(shared_per_SM / shared_per_block) |
| Block limit | hardware max (e.g., 32) |
| Warp limit | floor(max_warps / warps_per_block) |

**Worked example:**
  256 threads/block, 32 regs/thread, 4KB shared
  GPU: 65536 regs, 96KB shared, 64 warps, 32 blocks

  Regs: 65536 / (32×256) = 8 blocks
  Shared: 98304 / 4096 = 24 blocks
  Blocks: 32
  Warps: 64 / 8 = 8 blocks
  → **8 blocks, 64 warps, 100% occupancy** (limited by regs AND warps)

---

## Little's Law (Memory-Level Parallelism)

**Concurrent requests needed = Bandwidth × Latency / Request_size**

Example:
  BW = 900 GB/s, Latency = 500 ns, Cache line = 128 bytes
  N = (900e9 × 500e-9) / 128 = 3,516 requests

  Need ≥ 3,516 concurrent warps across all SMs to saturate bandwidth.
  At 64 warps/SM × 108 SMs = 6,912 → sufficient (if fully occupied).

---

## Performance Prediction

**predicted_time = max(memory_time, compute_time) + launch_overhead**

Where:
  memory_time = total_bytes / (achievable_BW)
  compute_time = FLOPs / (achievable_GFLOPS)
  launch_overhead ≈ 5-20 µs

---

## Worked Numeric Example: Vector Add, 1M elements, A100

- Bytes: 12 × 1M × 4 = 48 MB (read 2 arrays, write 1)
  Wait, N=1M elements × 4 bytes = 4MB per array.
  Read: 2 × 4MB = 8MB. Write: 4MB. Total: 12MB.
- FLOPs: 1M
- AI = 1M / 12M = 0.083 FLOP/byte
- Ridge point ≈ 12.5 FLOP/byte → **memory-bound**
- Memory time = 12MB / (1,555 × 0.85 GB/s) = 12e6 / 1.32e9 ≈ 9.1 µs
- Compute time = 1M / (19,468 × 0.7 GFLOPS) ≈ 0.07 µs
- Predicted ≈ max(9.1, 0.07) + 5 ≈ **14.1 µs**

---

## Key Ratios to Remember

| Metric | Typical Value |
|--------|--------------|
| Achievable BW fraction | 80-90% |
| Achievable FLOPS fraction | 60-80% |
| Memory latency (GDDR6) | 400-600 ns |
| Memory latency (HBM2) | 200-400 ns |
| L1 latency | ~30 cycles |
| L2 latency | ~200 cycles |
| Kernel launch overhead | 5-20 µs |
| Warp size | 32 (always on NVIDIA) |
