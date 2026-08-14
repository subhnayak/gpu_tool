# P3 — CUDA Ladder: 10 Rungs from Vector Add to Occupancy Tuning

A progressive sequence of CUDA kernels, each teaching one performance lesson.
Every rung verifies correctness against a CPU reference, times itself with
CUDA events, reports achieved bandwidth or GFLOP/s, and prints what percentage
of the hardware's theoretical peak it achieved.

---

## Before You Start: Install the CUDA Toolkit

> **Your GPU driver is already installed** (verified by `nvidia-smi`), but the
> driver and the **CUDA Toolkit** are separate things.  The driver lets you run
> CUDA programs; the toolkit gives you the tools to **build** them.

### What the toolkit provides

| Tool | Purpose |
|------|---------|
| `nvcc` | CUDA compiler (wraps the host compiler) |
| Nsight Compute (`ncu`) | Kernel-level profiler — the most important profiling tool |
| Nsight Systems (`nsys`) | Timeline profiler — shows overlap, transfers, launches |
| `cuobjdump` | Inspect compiled CUDA binaries |
| `nvdisasm` | Disassemble GPU code (SASS) |
| Headers & libraries | `cuda_runtime.h`, `cublas`, etc. |

### Installation steps

1. Download from **https://developer.nvidia.com/cuda-downloads**.
   Select: Windows → x86_64 → your OS version → exe (local).
2. During install, **uncheck** the driver component if your existing driver
   (581.42) is newer than what the toolkit bundles — the installer will tell
   you.  Never downgrade your driver.
3. **On Windows, `nvcc` requires MSVC (`cl.exe`) as the host compiler.**
   Install **Visual Studio Build Tools** (or full Visual Studio) with the
   **"Desktop development with C++"** workload.  The Community edition is free.
   - Direct link: https://visualstudio.microsoft.com/downloads/ → "Build Tools"
   - Ensure the "MSVC v14x" and "Windows SDK" components are checked.
4. After install, open a **new** terminal (or Developer Command Prompt for VS).

### Verify the install

```bash
# Should print version info (e.g., "release 12.x"):
nvcc --version

# Quick smoke test — build and run Rung 1:
nvcc -O3 -arch=sm_89 -std=c++17 -I. 01_vector_add.cu -o 01_vector_add.exe
.\01_vector_add.exe
# This prints your GPU's specs and runs a bandwidth test.
```

If `nvcc --version` works and the smoke test prints device info matching your
RTX 4000 Ada, you're ready.

---

## Learning Objectives

| # | Rung | Lesson | Interview Question |
|---|------|--------|--------------------|
| 01 | Vector Add | Memory-bound baseline; grid-stride loops | "What determines vector-add performance?" |
| 02 | Coalescing | Coalesced vs strided vs random access | "Explain memory coalescing" |
| 03 | Transpose | Shared memory tiling; bank conflicts | "How do you avoid bank conflicts?" |
| 04 | Reduction | 7-step optimization; warp shuffles; Volta ITS | "Optimize a parallel reduction" |
| 05 | Scan | Hillis-Steele vs Blelloch; multi-block scan | "Implement prefix sum on a GPU" |
| 06 | Histogram | Atomics; privatization | "How do atomics affect performance?" |
| 07 | SGEMM | Tiling; register blocking; arithmetic intensity | "Optimize matrix multiply" |
| 08 | Radix Sort | Composing scan+scatter primitives | "How does radix sort map to GPUs?" |
| 09 | Streams | Pinned memory; overlap; CUDA graphs | "How do you overlap transfer and compute?" |
| 10 | Occupancy | Occupancy vs ILP tradeoff (Volkov) | "Is max occupancy always optimal?" |

---

## Build Instructions

### Prerequisites

- CUDA Toolkit (11.0+)
- C++ compiler: MSVC (Windows) or GCC/Clang (Linux)
- CMake 3.18+ (optional but recommended)

### Option A: CMake (recommended)

```bash
# Windows (MSVC) — defaults to sm_89 (Ada)
cmake -B build -S .
cmake --build build --config Release

# Override architecture if needed:
cmake -B build -S . -DCMAKE_CUDA_ARCHITECTURES=native
# or for multiple archs:
cmake -B build -S . -DCMAKE_CUDA_ARCHITECTURES="86;89"

# Linux
cmake -B build -S .
cmake --build build

# Run all
cmake --build build --target RUN_ALL
```

### Option B: Direct nvcc (sm_89 for RTX 4000 Ada)

```bash
# Windows (MSVC + nvcc)
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 01_vector_add.cu -o 01_vector_add.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 02_coalescing.cu -o 02_coalescing.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 03_transpose.cu -o 03_transpose.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 04_reduction.cu -o 04_reduction.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 05_scan.cu -o 05_scan.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 06_histogram.cu -o 06_histogram.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 07_sgemm.cu -o 07_sgemm.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 08_radix_sort.cu -o 08_radix_sort.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 09_streams.cu -o 09_streams.exe
nvcc -O3 -arch=sm_89 --generate-line-info -Xptxas=-v -std=c++17 -I. 10_occupancy.cu -o 10_occupancy.exe

# With cuBLAS (optional, for 07_sgemm):
nvcc -O3 -arch=sm_89 --generate-line-info -std=c++17 -I. -DUSE_CUBLAS 07_sgemm.cu -o 07_sgemm.exe -lcublas

# Linux: replace .exe with no extension, same flags.
```

---

## Finding Your GPU's Theoretical Peak

Run any rung — each prints device info at startup.  Or use `deviceQuery` from
the CUDA samples.

### Formulas

**Peak Memory Bandwidth (GB/s):**
```
bandwidth = memoryClockRate_Hz × (memoryBusWidth_bits / 8) × 2 (DDR) / 1e9
```

**Peak FP32 Throughput (GFLOP/s):**
```
FP32_peak = numSMs × coresPerSM × 2 (FMA counts as 2 ops) × gpuClock_Hz / 1e9
```

### Worked Example: RTX 4000 Ada Generation (AD104, sm_89)

**Approximate** specs from public data sheets (your actual clocks may differ!):

| Parameter | Approximate value |
|-----------|-------------------|
| CUDA cores | 6144 (= 48 SMs × 128 cores/SM) |
| GPU boost clock | ~2.18 GHz |
| Memory | 20 GB GDDR6, 256-bit bus |
| Memory clock | ~10,001 MHz effective |

**Bandwidth** (approximate):
```
10001 MHz × 1e6 × (256/8) × 2 / 1e9 ≈ 640 GB/s
```

**FP32 peak** (approximate):
```
48 SMs × 128 cores × 2 × 2.18 GHz ≈ 26,726 GFLOP/s
```

> **⚠️ IMPORTANT**: These are **approximate** spec-sheet numbers.  Your actual
> GPU may boost differently.  **Always verify from your own `deviceQuery` output**
> (or the device-info printout from any rung).  The `queryDevice()` function in
> `common/helpers.cuh` computes both peaks from the CUDA runtime's reported
> clocks.  Building the habit of *never trusting spec sheets* and always measuring
> is itself the lesson.

### Ada Architecture Notes (sm_89)

- **128 KB combined L1 / shared memory per SM** with a configurable split
  (driver selects, or set via `cudaFuncSetAttribute` with
  `cudaFuncAttributePreferredSharedMemoryCarveout`).  This is larger than
  Ampere's 128 KB (GA10x) and significantly larger than Turing's 96 KB.
- **Large L2 cache**: Ada substantially increased L2 vs Ampere.  The RTX 4000
  Ada has ~32 MB of L2.  You'll see this in Exercise E2 (L2 vs DRAM bandwidth):
  working sets up to ~32 MB will show elevated effective bandwidth.
- **4th-generation Tensor Cores**: support FP8, FP16, BF16, TF32, INT8.
  Exercise E8 (WMMA) can use these.
- **`cp.async` (async copy)**: sm_80+ supports `cp.async` for direct
  global→shared memory copies that bypass registers.  This is the foundation
  of modern tiling patterns in CUTLASS and can be explored as an extension
  exercise.
- **Thread Block Clusters**: sm_90+ (Hopper) only — not available on sm_89.

---

## Profiling with Nsight Tools

### Nsight Compute (kernel-level analysis)

```bash
# Full metrics for a single rung:
ncu --set full -o report_coalescing ./02_coalescing

# Specific metrics:
ncu --metrics dram__bytes.sum.per_second,sm__throughput.avg.pct_of_peak_sustained_elapsed ./01_vector_add

# Bank conflicts (transpose):
ncu --metrics l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_ld.sum ./03_transpose

# Stall reasons (occupancy):
ncu --metrics smsp__warps_launched.sum,smsp__average_warps_issue_stalled_long_scoreboard_per_issue_active.ratio ./10_occupancy

# L1 hit rate:
ncu --metrics l1tex__t_sector_hit_rate.pct ./02_coalescing
```

### Nsight Systems (timeline / overlap)

```bash
# Streams overlap analysis:
nsys profile -o streams_report ./09_streams

# General timeline:
nsys profile -o ladder_report ./01_vector_add

# Open in Nsight Systems GUI to see H2D, kernel, D2H bars.
```

### Key Metrics by Rung

| Rung | Key ncu Metrics |
|------|----------------|
| 01 | `dram__bytes.sum.per_second`, `sm__throughput` |
| 02 | `dram__bytes.sum.per_second`, `l1tex__t_sector_hit_rate` |
| 03 | `l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_ld.sum` |
| 04 | `smsp__warps_launched`, stall reasons |
| 05 | `dram__bytes.sum.per_second` |
| 06 | `l1tex__t_request_hit_rate`, atomic throughput |
| 07 | `sm__throughput`, `smsp__inst_executed_pipe_fma` |
| 08 | `dram__bytes.sum.per_second` |
| 09 | Use `nsys` for timeline overlap |
| 10 | `sm__warps_active.avg.pct_of_peak_sustained_active`, `smsp__inst_executed_pipe_fma` |

---

## Your Results Table

Fill this in with your own measurements:

| Rung | Kernel | Time (ms) | Throughput | Unit | % Peak | Bottleneck |
|------|--------|-----------|------------|------|--------|------------|
| 01 | vectorAddSimple | | | GB/s | | |
| 01 | vectorAddGridStride | | | GB/s | | |
| 02 | stride=1 | | | GB/s | | |
| 02 | stride=32 | | | GB/s | | |
| 02 | random | | | GB/s | | |
| 02 | float4 | | | GB/s | | |
| 03 | copy (upper bound) | | | GB/s | | |
| 03 | naive transpose | | | GB/s | | |
| 03 | tiled + padded | | | GB/s | | |
| 04 | v1 (divergent) | | | GB/s | | |
| 04 | v7 (shuffle) | | | GB/s | | |
| 05 | multi-block scan | | | GB/s | | |
| 06 | naive (uniform) | | | GB/s | | |
| 06 | shared (uniform) | | | GB/s | | |
| 07 | naive SGEMM | | | GFLOP/s | | |
| 07 | tiled SGEMM | | | GFLOP/s | | |
| 07 | register-tiled | | | GFLOP/s | | |
| 08 | radix sort | | | GB/s | | |
| 09 | pageable H2D | | | GB/s | | |
| 09 | pinned H2D | | | GB/s | | |
| 09 | sequential | | | ms | | |
| 09 | pipelined | | | ms | | |
| 10 | ILP depth=1 | | | GFLOP/s | | |
| 10 | ILP depth=8 | | | GFLOP/s | | |

Use `python run_all.py` to auto-populate this table.

---

## Acceptance Criteria

- [ ] Every kernel verifies correct against CPU reference before timing
- [ ] Every rung profiled with `ncu` or `nsys`
- [ ] % of roofline documented for each kernel
- [ ] Results table filled in with your hardware's numbers
- [ ] 12 EXERCISES.md exercises attempted (at least 6)

---

## Questions to Answer After Finishing

These map directly to NVIDIA interview questions:

1. **Why is vector-add memory-bound?** Calculate its arithmetic intensity and
   show where it falls on the roofline.

2. **What happens to bandwidth when stride doubles?** Explain in terms of
   cache-line utilization and sector requests.

3. **Why does `float tile[32][33]` eliminate bank conflicts?** Draw the bank
   mapping for 32 vs 33 columns.

4. **Walk through the 7 reduction optimizations.** For each, name the bottleneck
   it removes and quantify the speedup.

5. **Why is warp-synchronous code broken on Volta?** Explain independent thread
   scheduling and the convergence barrier.

6. **What is `__shfl_down_sync` and why does it need a mask?** Explain the
   cooperative semantics.

7. **How does shared-memory privatization reduce atomic contention?**
   Why is the speedup larger with skewed distributions?

8. **What determines whether a kernel is memory-bound or compute-bound?**
   Define arithmetic intensity and the roofline ridge point.

9. **How do you overlap H2D, compute, and D2H?** Draw the timeline with
   sequential vs 4-stream pipelined execution.

10. **Is maximum occupancy always optimal?** Explain the Volkov argument
    with your ILP-depth results.

11. **What is a CUDA graph and when would you use one?** Cite your launch-
    overhead numbers.

12. **How would you profile a kernel you suspect has bank conflicts?**
    Name the exact `ncu` metric and what value means "no conflicts."

---

## Project Structure

```
P3-cuda-ladder/
├── README.md              ← You are here
├── CMakeLists.txt         ← Build system
├── common/
│   └── helpers.cuh        ← Error checking, timing, verification, device info
├── 01_vector_add.cu       ← Rung 1: Memory-bound baseline
├── 02_coalescing.cu       ← Rung 2: Coalescing patterns
├── 03_transpose.cu        ← Rung 3: Bank conflicts
├── 04_reduction.cu        ← Rung 4: 7-step reduction
├── 05_scan.cu             ← Rung 5: Prefix sum
├── 06_histogram.cu        ← Rung 6: Atomics
├── 07_sgemm.cu            ← Rung 7: Matrix multiply
├── 08_radix_sort.cu       ← Rung 8: Composing primitives
├── 09_streams.cu          ← Rung 9: Overlap & graphs
├── 10_occupancy.cu        ← Rung 10: Occupancy vs ILP
├── run_all.py             ← Auto-run + results table generator
└── EXERCISES.md           ← 12 extension exercises
```
