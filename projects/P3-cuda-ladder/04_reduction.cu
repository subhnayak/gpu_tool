// 04_reduction.cu — Rung 4: Parallel Reduction (7 optimization steps)
// ============================================================================
// The classic progression from Mark Harris's "Optimizing Parallel Reduction
// in CUDA."  Each version fixes one bottleneck.
//
// INTERVIEW QUESTIONS:
//   - "Walk me through optimizing a parallel reduction on a GPU."
//   - "What changed about warp-synchronous programming at Volta?"
//   - "When would you use warp shuffles vs shared memory?"
//
// EXPECTED: v7 (shuffle) achieves near-peak memory bandwidth.  The kernel is
// memory-bound (1 FLOP per element, 4 bytes loaded) so bandwidth is the metric.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "common/helpers.cuh"

// Problem size.
static const int N = 1 << 24;  // 16M elements
static const int TPB = 256;    // threads per block

// CPU reference.
static float reduceCPU(const float* data, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += data[i];
    return (float)sum;
}

// ============================================================================
// v1: Interleaved addressing with DIVERGENT branching (and modulo).
// Thread k checks if (k % (2*s) == 0) → divergent within a warp.
// Also suffers from shared-memory bank conflicts due to strided access.
// ============================================================================
__global__ void reduceV1(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    int tid = threadIdx.x;
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    sdata[tid] = (i < n) ? g_in[i] : 0.0f;
    __syncthreads();

    for (int s = 1; s < blockDim.x; s *= 2) {
        // PROBLEM: only threads where (tid % (2*s) == 0) do work.
        // This causes warp divergence AND strided shared-mem access.
        if (tid % (2 * s) == 0) {
            sdata[tid] += sdata[tid + s];
        }
        __syncthreads();
    }
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v2: Interleaved addressing, NON-DIVERGENT branching.
// We replace the modulo test with a contiguous thread check.
// Still has bank conflicts (stride doubles each iteration).
// ============================================================================
__global__ void reduceV2(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    int tid = threadIdx.x;
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    sdata[tid] = (i < n) ? g_in[i] : 0.0f;
    __syncthreads();

    for (int s = 1; s < blockDim.x; s *= 2) {
        int index = 2 * s * tid;
        if (index + s < blockDim.x) {
            sdata[index] += sdata[index + s];
        }
        __syncthreads();
    }
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v3: SEQUENTIAL addressing.
// Iterate from blockDim/2 down.  Each thread adds element at tid+s.
// This eliminates bank conflicts (contiguous access) and warp divergence
// (inactive threads are at the end of each warp).
// ============================================================================
__global__ void reduceV3(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    int tid = threadIdx.x;
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    sdata[tid] = (i < n) ? g_in[i] : 0.0f;
    __syncthreads();

    for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sdata[tid] += sdata[tid + s];
        }
        __syncthreads();
    }
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v4: First add during global load.
// Each thread loads TWO elements and adds them, halving the number of blocks.
// This doubles the useful work done while data is in flight from global memory.
// ============================================================================
__global__ void reduceV4(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    int tid = threadIdx.x;
    int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;

    float mySum = 0.0f;
    if (i < n)                mySum  = g_in[i];
    if (i + blockDim.x < n)   mySum += g_in[i + blockDim.x];
    sdata[tid] = mySum;
    __syncthreads();

    for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sdata[tid] += sdata[tid + s];
        }
        __syncthreads();
    }
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v5: Unroll the last warp.
// When s <= 32 (warp size), ALL active threads are in the same warp, so
// __syncthreads() is unnecessary.  We unroll those iterations manually.
//
// *** WARNING ***: Pre-Volta (compute < 7.0) GPUs execute warps in lock-step,
// so the `volatile` keyword on shared memory was enough.  Post-Volta
// (independent thread scheduling), threads in a warp can diverge, so this
// warp-synchronous code is UNSAFE.  We keep it here for historical education
// and mark it clearly.
//
// Interview tie-in: "What is independent thread scheduling and why did it
// break warp-synchronous code?"
// ============================================================================
__device__ void warpReduceUnsafe(volatile float* sdata, int tid) {
    // volatile prevents the compiler from optimising away loads/stores.
    // This is ONLY correct on pre-Volta GPUs with lock-step warp execution!
    sdata[tid] += sdata[tid + 32];
    sdata[tid] += sdata[tid + 16];
    sdata[tid] += sdata[tid + 8];
    sdata[tid] += sdata[tid + 4];
    sdata[tid] += sdata[tid + 2];
    sdata[tid] += sdata[tid + 1];
}

__global__ void reduceV5(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    int tid = threadIdx.x;
    int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;

    float mySum = 0.0f;
    if (i < n)                mySum  = g_in[i];
    if (i + blockDim.x < n)   mySum += g_in[i + blockDim.x];
    sdata[tid] = mySum;
    __syncthreads();

    for (unsigned int s = blockDim.x / 2; s > 32; s >>= 1) {
        if (tid < s) sdata[tid] += sdata[tid + s];
        __syncthreads();
    }
    if (tid < 32) warpReduceUnsafe(sdata, tid);
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v6: Complete unroll with template on block size.
// The compiler knows blockDim at compile time → it can fully unroll the loop
// and remove dead branches.  This eliminates loop overhead entirely.
// Still uses shared memory, still warp-synchronous in the last warp.
// ============================================================================
template <unsigned int blockSize>
__global__ void reduceV6(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    extern __shared__ float sdata[];
    unsigned int tid = threadIdx.x;
    unsigned int i = blockIdx.x * (blockSize * 2) + threadIdx.x;

    float mySum = 0.0f;
    if (i < n)            mySum  = g_in[i];
    if (i + blockSize < (unsigned int)n) mySum += g_in[i + blockSize];
    sdata[tid] = mySum;
    __syncthreads();

    // Fully unrolled: the compiler removes branches where blockSize < s.
    if (blockSize >= 512) { if (tid < 256) sdata[tid] += sdata[tid + 256]; __syncthreads(); }
    if (blockSize >= 256) { if (tid < 128) sdata[tid] += sdata[tid + 128]; __syncthreads(); }
    if (blockSize >= 128) { if (tid <  64) sdata[tid] += sdata[tid +  64]; __syncthreads(); }

    // Last-warp unroll (warp-synchronous).
    if (tid < 32) {
        volatile float* vs = sdata;
        if (blockSize >= 64) vs[tid] += vs[tid + 32];
        if (blockSize >= 32) vs[tid] += vs[tid + 16];
        if (blockSize >= 16) vs[tid] += vs[tid + 8];
        if (blockSize >=  8) vs[tid] += vs[tid + 4];
        if (blockSize >=  4) vs[tid] += vs[tid + 2];
        if (blockSize >=  2) vs[tid] += vs[tid + 1];
    }
    if (tid == 0) g_out[blockIdx.x] = sdata[0];
}

// ============================================================================
// v7: Warp shuffle + atomicAdd — the MODERN approach.
// __shfl_down_sync replaces shared memory for intra-warp reductions.
// This is correct on ALL architectures (we pass the explicit mask 0xffffffff).
// After reducing to one value per warp, we use shared memory to collect warp
// results, then one final warp-shuffle reduce, and atomicAdd to the output.
//
// This is the RECOMMENDED approach for Volta+ GPUs.
// ============================================================================
__device__ float warpReduceShuffle(float val) {
    // Full warp mask — all 32 threads participate.
    for (int offset = 16; offset > 0; offset >>= 1) {
        val += __shfl_down_sync(0xffffffff, val, offset);
    }
    return val;
}

__global__ void reduceV7(const float* __restrict__ g_in, float* __restrict__ g_out, int n) {
    // Each thread loads two elements (like v4).
    int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;
    float mySum = 0.0f;
    if (i < n)                mySum  = g_in[i];
    if (i + blockDim.x < n)   mySum += g_in[i + blockDim.x];

    // Warp-level reduction using shuffles.
    mySum = warpReduceShuffle(mySum);

    // Each warp's lane 0 writes to shared memory.
    __shared__ float warpSums[32];  // max 32 warps per block (1024/32)
    int lane = threadIdx.x % 32;
    int warpId = threadIdx.x / 32;

    if (lane == 0) warpSums[warpId] = mySum;
    __syncthreads();

    // First warp reduces the warp sums.
    int numWarps = blockDim.x / 32;
    mySum = (threadIdx.x < (unsigned)numWarps) ? warpSums[threadIdx.x] : 0.0f;
    if (warpId == 0) {
        mySum = warpReduceShuffle(mySum);
    }

    // One atomicAdd per block to the global output.
    if (threadIdx.x == 0) {
        atomicAdd(g_out, mySum);
    }
}

// ============================================================================
// Runner
// ============================================================================
struct ReduceVariant {
    const char* name;
    // We'll dispatch via function pointers and a type tag.
    int version;
};

int main() {
    printf("\n===== RUNG 4: Parallel Reduction =====\n\n");

    DeviceInfo info = queryDevice();

    const size_t bytes = N * sizeof(float);
    // Bandwidth metric: we load N floats from global memory.
    // The reduction is memory-bound; the adds are free.
    const double totalBytes = (double)bytes;

    float* h_data = (float*)malloc(bytes);
    for (int i = 0; i < N; ++i) h_data[i] = 1.0f;  // sum = N exactly

    float cpuSum = reduceCPU(h_data, N);
    printf("  CPU reference sum = %.0f  (expected %d)\n\n", cpuSum, N);

    float *d_in, *d_out;
    CUDA_CHECK(cudaMalloc(&d_in, bytes));
    CUDA_CHECK(cudaMemcpy(d_in, h_data, bytes, cudaMemcpyHostToDevice));

    // d_out: enough for partial sums (one per block).
    int blocksV1 = (N + TPB - 1) / TPB;
    int blocksV4 = (N + TPB * 2 - 1) / (TPB * 2);
    int maxBlocks = blocksV1;
    CUDA_CHECK(cudaMalloc(&d_out, maxBlocks * sizeof(float)));

    // Partial sums buffer (host).
    float* h_partial = (float*)malloc(maxBlocks * sizeof(float));

    // Helper: two-pass reduce (v1-v6 produce per-block results; sum on CPU).
    auto twoPassSum = [&](float* d_partial, int nblocks) -> float {
        float* hp = (float*)malloc(nblocks * sizeof(float));
        CUDA_CHECK(cudaMemcpy(hp, d_partial, nblocks * sizeof(float), cudaMemcpyDeviceToHost));
        double s = 0.0;
        for (int i = 0; i < nblocks; ++i) s += hp[i];
        free(hp);
        return (float)s;
    };

    size_t smemBytes = TPB * sizeof(float);

    float baselineMs = 0.0f;

    printf("  %-45s %8s  %8s  %7s  %7s\n",
           "Variant", "ms", "GB/s", "% Peak", "Speedup");
    printf("  %-45s %8s  %8s  %7s  %7s\n",
           "-------", "--", "----", "------", "-------");

    // Macro to bench and print a variant.
    // For v1-v6 we do two-pass (block reduce + CPU sum).
    // For v7 we use atomicAdd (single output).
    auto benchVariant = [&](const char* name, auto launchFn, int nblocks, bool usesAtomic) {
        // Verify.
        CUDA_CHECK(cudaMemset(d_out, 0, maxBlocks * sizeof(float)));
        launchFn();
        CUDA_CHECK_KERNEL();
        float gpuSum;
        if (usesAtomic) {
            CUDA_CHECK(cudaMemcpy(&gpuSum, d_out, sizeof(float), cudaMemcpyDeviceToHost));
        } else {
            gpuSum = twoPassSum(d_out, nblocks);
        }
        float err = fabsf(gpuSum - cpuSum) / fabsf(cpuSum);
        if (err > 1e-3f) {
            printf("  FAIL: %s  got %.0f expected %.0f (err=%.2e)\n",
                   name, gpuSum, cpuSum, err);
            return;
        }

        BenchResult res = benchmark([&](GpuTimer& t) {
            CUDA_CHECK(cudaMemset(d_out, 0, maxBlocks * sizeof(float)));
            t.start();
            launchFn();
            t.stop();
        });

        if (baselineMs == 0.0f) baselineMs = res.medianMs;
        double secs = res.medianMs / 1000.0;
        double gbps = totalBytes / secs / 1e9;
        double pct  = 100.0 * gbps / info.peakBandwidthGBs;
        double speedup = baselineMs / res.medianMs;
        printf("  %-45s %8.3f  %8.1f  %6.1f%%  %6.2fx\n",
               name, res.medianMs, gbps, pct, speedup);
    };

    // v1
    benchVariant("v1: interleaved, divergent (modulo)", [&]() {
        reduceV1<<<blocksV1, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV1, false);

    // v2
    benchVariant("v2: interleaved, non-divergent", [&]() {
        reduceV2<<<blocksV1, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV1, false);

    // v3
    benchVariant("v3: sequential addressing", [&]() {
        reduceV3<<<blocksV1, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV1, false);

    // v4
    benchVariant("v4: first add during load (half blocks)", [&]() {
        reduceV4<<<blocksV4, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV4, false);

    // v5
    benchVariant("v5: unroll last warp (volatile)", [&]() {
        reduceV5<<<blocksV4, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV4, false);

    // v6
    benchVariant("v6: fully unrolled (template)", [&]() {
        reduceV6<TPB><<<blocksV4, TPB, smemBytes>>>(d_in, d_out, N);
    }, blocksV4, false);

    // v7
    benchVariant("v7: warp shuffle + atomicAdd", [&]() {
        reduceV7<<<blocksV4, TPB>>>(d_in, d_out, N);
    }, 1, true);

    printf("\n");
    printf("  IMPORTANT (Volta+ / Independent Thread Scheduling):\n");
    printf("  v5 uses 'volatile' for warp-synchronous reduction.\n");
    printf("  Post-Volta, threads in a warp have INDEPENDENT program counters.\n");
    printf("  This means warp-synchronous code (relying on lock-step execution)\n");
    printf("  is UNSAFE.  v7 uses __shfl_down_sync with explicit mask 0xffffffff,\n");
    printf("  which is correct on ALL architectures.\n\n");

    CUDA_CHECK(cudaFree(d_in));
    CUDA_CHECK(cudaFree(d_out));
    free(h_data); free(h_partial);

    return 0;
}
