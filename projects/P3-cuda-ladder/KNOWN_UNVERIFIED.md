# Known Unverified Items

**Compilation was not machine-verified** — `nvcc` was not installed on the
development machine.  All code was written and reviewed by hand.  This file
lists what to watch for on first compile.

## Bugs Found and Fixed (pre-ship review)

1. **`helpers.cuh` — `%zu` format specifier**: MSVC's `printf` doesn't
   reliably support `%zu` for `size_t`.  Fixed: cast to `(unsigned)` and
   use `%u`.

2. **`04_reduction.cu` v2 — shared memory out-of-bounds read**: The guard
   `if (index < blockDim.x)` did not prevent `sdata[index + s]` from reading
   beyond the shared memory allocation when `index + s >= blockDim.x`.
   Fixed: changed guard to `if (index + s < blockDim.x)`.

3. **`05_scan.cu` Blelloch — shared memory under-allocation**: The kernel
   rounds `n` up to `pow2` internally and accesses `temp[pow2 - 1]`, but
   callers allocated only `n * sizeof(float)` of shared memory.  When
   `pow2 > n`, this was an out-of-bounds access.  Fixed: callers now compute
   `pow2` and allocate `pow2 * sizeof(float)`.  Also fixed the kernel to
   guard the initial zero-fill against the `pow2` bound (not `blockDim.x`).

4. **`09_streams.cu` — `cudaGraphInstantiate` deprecation**: The 5-argument
   overload is deprecated in CUDA 12+.  Fixed: added `#if CUDART_VERSION`
   guard to use the 2-argument form on CUDA 12+.

## Things to Watch on First Compile

- **`extern __shared__` type aliasing**: `04_reduction.cu` and `05_scan.cu`
  both use `extern __shared__ float sdata[]` / `temp[]`.  Since each file has
  only one kernel using extern shared (or each kernel is in a separate
  compilation unit), there is no aliasing conflict.  If you add a kernel that
  uses `extern __shared__ int` in the same file, use the `extern __shared__
  char smem[]` + `reinterpret_cast` idiom.

- **`06_histogram.cu`**: Uses `extern __shared__ int s_hist[]` in two kernels
  (`histogramShared` and `histogramLargeBins`).  Both are `int`, so no
  aliasing issue.  Fine.

- **MSVC C++17 lambda in CUDA**: The benchmark runner uses host-side lambdas
  with device code captures (`[&](...) { kernel<<<...>>>(...); }`).  This
  works with `--extended-lambda` which nvcc enables by default for C++17.
  If you see errors about "lambda in device code," add `--extended-lambda`.

- **`__launch_bounds__` with templates** (`10_occupancy.cu`):  The attribute
  `__launch_bounds__(256, 4)` is placed after `__global__` and before the
  function name.  Some older nvcc versions require it between `void` and the
  function name.  If compilation fails, move it: 
  `__global__ void __launch_bounds__(256, 4) computeHighOcc(...)`.
  (The current placement should be correct for CUDA 11+.)

- **`std::srand` / `rand()`** (`07_sgemm.cu`): Used on host only for
  initialisation.  No device-side usage.

- **Integer overflow in index math**: All kernels use `int` indices.  The
  largest problem size is 16M elements (Rung 1/4), and the largest index
  computation is in `02_coalescing.cu` with `idx * 32` where `idx < 4M`,
  giving ~128M — well within `int` range.  For sizes beyond ~500M elements,
  switch to `long long` indices.

- **`07_sgemm.cu` register-tiled kernel**: The cooperative loading loop
  (`for (int i = tid; i < TILE * BK; ...)`) with 64 threads loading 1024
  elements means each thread loads 16 elements.  This is correct but
  register-heavy.  If nvcc reports excessive register usage and spilling,
  try reducing `BK` to 16.

- **Radix sort stability**: The scatter kernel uses `atomicAdd` on shared
  memory to allocate positions within a block.  This makes the sort
  non-stable (elements with the same key within a block may be reordered).
  The output is still correctly sorted; it just isn't a stable sort.
  Verification against `std::sort` (which produces a sorted output regardless
  of stability) will pass.

## Not Tested

- cuBLAS path in `07_sgemm.cu` (guarded by `#ifdef USE_CUBLAS`).
- Cross-platform Linux build (designed to work but not tested).
- CUDA Toolkit versions older than 11.0.
- GPUs other than sm_89 (the code should work on sm_60+ but was designed
  for Ada).
