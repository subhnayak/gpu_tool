// 02_coalescing.cu — Rung 2: Memory Coalescing
// ============================================================================
// THE most important performance lesson in CUDA: memory coalescing.
//
// When threads in a warp access CONSECUTIVE memory addresses, the hardware
// combines (coalesces) those accesses into a minimal number of cache-line
// transactions.  When accesses are strided or random, each thread may trigger
// a separate cache-line fetch, wasting bandwidth by a factor up to 32×.
//
// INTERVIEW QUESTION: "Explain memory coalescing and its impact on bandwidth."
// This rung gives you concrete numbers to cite in your answer.
//
// We run the SAME total work (copy N floats) with different access patterns:
//   1. Perfectly coalesced (stride 1) — baseline.
//   2. Strided: stride 2, 4, 8, 16, 32 — watch bandwidth collapse.
//   3. Offset (misaligned) by 1..32 elements — shows cache-line waste.
//   4. Random (permuted) access — worst case.
//   5. Vectorized float4 loads — can exceed scalar coalesced on some GPUs.
//
// EXPECTED: Coalesced ≈ 80-90% peak.  Stride-32 ≈ 1/32 of coalesced.
//           Random ≈ even worse.  float4 ≈ coalesced or slightly better.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <random>
#include "common/helpers.cuh"

// ---- Strided copy kernel ----
// Thread i reads from src[i * stride] and writes to dst[i * stride].
// Total elements touched = N; total UNIQUE elements = N; but the access
// pattern determines how many cache lines are fetched.
__global__ void stridedCopy(const float* __restrict__ src,
                            float* __restrict__ dst,
                            int N, int stride) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N) {
        int addr = idx * stride;
        // Guard: addr must be within allocation (caller allocates N*stride).
        dst[addr] = src[addr];
    }
}

// ---- Offset (misaligned) copy kernel ----
// All threads access contiguous elements, but starting from an offset.
// This tests misalignment relative to cache-line boundaries (128 bytes).
__global__ void offsetCopy(const float* __restrict__ src,
                           float* __restrict__ dst,
                           int N, int offset) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N) {
        dst[idx + offset] = src[idx + offset];
    }
}

// ---- Random (permuted) copy kernel ----
// Each thread reads from a random (pre-permuted) location.
__global__ void randomCopy(const float* __restrict__ src,
                           float* __restrict__ dst,
                           const int* __restrict__ perm,
                           int N) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N) {
        int p = perm[idx];
        dst[idx] = src[p];
    }
}

// ---- Vectorized float4 copy kernel ----
// Each thread loads/stores 4 consecutive floats at once.
// This uses 128-bit transactions and can sometimes exceed scalar coalesced
// bandwidth because it reduces the number of instructions.
__global__ void vectorizedCopy(const float4* __restrict__ src,
                               float4* __restrict__ dst,
                               int N4) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < N4) {
        dst[idx] = src[idx];
    }
}

// CPU reference: simple copy.
void copyCPU(const float* src, float* dst, int N) {
    for (int i = 0; i < N; ++i) dst[i] = src[i];
}

int main() {
    printf("\n===== RUNG 2: Memory Coalescing =====\n\n");

    DeviceInfo info = queryDevice();

    // N must be large enough to saturate bandwidth.  We allocate extra space
    // for strided access (N * maxStride elements).
    const int N = 1 << 22;  // 4M elements
    const int maxStride = 32;
    const size_t allocBytes = (size_t)N * maxStride * sizeof(float);
    const size_t copyBytes  = (size_t)N * sizeof(float);
    // Effective traffic for a copy: 1 load + 1 store = 2 arrays.
    const double totalBytes = 2.0 * copyBytes;

    const int TPB = 256;
    const int blocks = (N + TPB - 1) / TPB;

    // Host data.
    float* h_src = (float*)malloc(allocBytes);
    float* h_dst = (float*)malloc(allocBytes);
    float* h_ref = (float*)malloc(allocBytes);
    for (int i = 0; i < N * maxStride; ++i) h_src[i] = sinf(i * 0.0001f);

    // Permutation for random access.
    int* h_perm = (int*)malloc(N * sizeof(int));
    for (int i = 0; i < N; ++i) h_perm[i] = i;
    std::mt19937 rng(42);
    std::shuffle(h_perm, h_perm + N, rng);

    // Device allocations.
    float *d_src, *d_dst;
    int *d_perm;
    CUDA_CHECK(cudaMalloc(&d_src, allocBytes));
    CUDA_CHECK(cudaMalloc(&d_dst, allocBytes));
    CUDA_CHECK(cudaMalloc(&d_perm, N * sizeof(int)));
    CUDA_CHECK(cudaMemcpy(d_src, h_src, allocBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_perm, h_perm, N * sizeof(int), cudaMemcpyHostToDevice));

    // ========================================================================
    // (1) STRIDED ACCESS — stride 1, 2, 4, 8, 16, 32
    // ========================================================================
    printf("  %-10s  %10s  %10s  %8s\n", "Pattern", "Time (ms)", "GB/s", "% Peak");
    printf("  %-10s  %10s  %10s  %8s\n", "-------", "---------", "----", "------");

    int strides[] = {1, 2, 4, 8, 16, 32};
    for (int s : strides) {
        // Verify correctness for this stride.
        CUDA_CHECK(cudaMemset(d_dst, 0, allocBytes));
        stridedCopy<<<blocks, TPB>>>(d_src, d_dst, N, s);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_dst, d_dst, allocBytes, cudaMemcpyDeviceToHost));

        // Build CPU reference for strided copy.
        memset(h_ref, 0, allocBytes);
        for (int i = 0; i < N; ++i) h_ref[i * s] = h_src[i * s];
        bool ok = true;
        for (int i = 0; i < N; ++i) {
            if (fabsf(h_ref[i * s] - h_dst[i * s]) > 1e-5f) { ok = false; break; }
        }
        if (!ok) {
            printf("  FAIL: stridedCopy stride=%d\n", s);
            return 1;
        }

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            stridedCopy<<<blocks, TPB>>>(d_src, d_dst, N, s);
            t.stop();
        });

        double secs = res.medianMs / 1000.0;
        double gbps = totalBytes / secs / 1e9;
        double pct  = 100.0 * gbps / info.peakBandwidthGBs;
        char label[64];
        snprintf(label, sizeof(label), "stride=%d", s);
        printf("  %-10s  %10.3f  %10.1f  %7.1f%%\n", label, res.medianMs, gbps, pct);
    }

    // ========================================================================
    // (2) OFFSET (MISALIGNED) ACCESS — offset 0, 1, 2, 4, 8, 16, 32
    // ========================================================================
    printf("\n  Misaligned offsets (elements):\n");
    printf("  %-10s  %10s  %10s  %8s\n", "Offset", "Time (ms)", "GB/s", "% Peak");
    printf("  %-10s  %10s  %10s  %8s\n", "------", "---------", "----", "------");

    int offsets[] = {0, 1, 2, 4, 8, 16, 32};
    // For offset tests we use N - maxStride elements to stay in bounds.
    const int Noff = N - maxStride;
    const int blocksOff = (Noff + TPB - 1) / TPB;
    const double totalBytesOff = 2.0 * Noff * sizeof(float);

    for (int off : offsets) {
        CUDA_CHECK(cudaMemset(d_dst, 0, allocBytes));
        offsetCopy<<<blocksOff, TPB>>>(d_src, d_dst, Noff, off);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_dst, d_dst, allocBytes, cudaMemcpyDeviceToHost));

        bool ok = true;
        for (int i = 0; i < Noff; ++i) {
            if (fabsf(h_src[i + off] - h_dst[i + off]) > 1e-5f) { ok = false; break; }
        }
        if (!ok) { printf("  FAIL: offsetCopy offset=%d\n", off); return 1; }

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            offsetCopy<<<blocksOff, TPB>>>(d_src, d_dst, Noff, off);
            t.stop();
        });

        double secs = res.medianMs / 1000.0;
        double gbps = totalBytesOff / secs / 1e9;
        double pct  = 100.0 * gbps / info.peakBandwidthGBs;
        char label[64];
        snprintf(label, sizeof(label), "offset=%d", off);
        printf("  %-10s  %10.3f  %10.1f  %7.1f%%\n", label, res.medianMs, gbps, pct);
    }

    // ========================================================================
    // (3) RANDOM ACCESS
    // ========================================================================
    printf("\n");
    {
        CUDA_CHECK(cudaMemset(d_dst, 0, copyBytes));
        randomCopy<<<blocks, TPB>>>(d_src, d_dst, d_perm, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_dst, d_dst, copyBytes, cudaMemcpyDeviceToHost));

        // CPU reference for random copy.
        for (int i = 0; i < N; ++i) h_ref[i] = h_src[h_perm[i]];
        if (!verifyFloats(h_ref, h_dst, N)) {
            printf("  FAIL: randomCopy\n"); return 1;
        }

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            randomCopy<<<blocks, TPB>>>(d_src, d_dst, d_perm, N);
            t.stop();
        });
        reportBandwidth("random (permuted)", res.medianMs,
                        totalBytes, info.peakBandwidthGBs);
    }

    // ========================================================================
    // (4) VECTORIZED float4
    // ========================================================================
    {
        int N4 = N / 4;
        int blocks4 = (N4 + TPB - 1) / TPB;

        CUDA_CHECK(cudaMemset(d_dst, 0, copyBytes));
        vectorizedCopy<<<blocks4, TPB>>>((float4*)d_src, (float4*)d_dst, N4);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_dst, d_dst, copyBytes, cudaMemcpyDeviceToHost));

        copyCPU(h_src, h_ref, N);
        if (!verifyFloats(h_ref, h_dst, N)) {
            printf("  FAIL: vectorizedCopy\n"); return 1;
        }

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            vectorizedCopy<<<blocks4, TPB>>>((float4*)d_src, (float4*)d_dst, N4);
            t.stop();
        });
        reportBandwidth("vectorized float4", res.medianMs,
                        totalBytes, info.peakBandwidthGBs);
    }

    printf("\n  KEY TAKEAWAY: Strided access at stride=32 uses ~1/32 of peak BW.\n");
    printf("  The memory system, not the ALUs, determines performance.\n");
    printf("  Profile with: ncu --set full -o coalescing ./02_coalescing\n");
    printf("  Look at: dram__bytes.sum.per_second, l1tex__t_sector_hit_rate\n\n");

    // Cleanup.
    CUDA_CHECK(cudaFree(d_src));
    CUDA_CHECK(cudaFree(d_dst));
    CUDA_CHECK(cudaFree(d_perm));
    free(h_src); free(h_dst); free(h_ref); free(h_perm);

    return 0;
}
