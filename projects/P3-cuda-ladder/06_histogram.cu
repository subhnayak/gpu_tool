// 06_histogram.cu — Rung 6: Histogram (Atomics and Privatization)
// ============================================================================
// LESSON: Atomic contention is a major bottleneck.  Privatization — giving
// each block its own copy of the histogram in shared memory, then merging —
// dramatically reduces contention.
//
// We implement:
//   (a) Naive global atomics — every thread atomicAdd's directly to global.
//   (b) Shared-memory privatized histogram — each block accumulates in smem,
//       then one merge pass to global.
//   (c) Large-bin variant — when #bins > shared memory capacity, we tile.
//
// We test with UNIFORM and SKEWED distributions:
//   - Uniform: contention is spread → naive isn't terrible.
//   - Skewed (most values in a few bins): contention is extreme → naive is
//     many times slower.
//
// INTERVIEW: "How do atomic operations affect performance?"
//            "What is histogram privatization?"
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <random>
#include "common/helpers.cuh"

static const int TPB = 256;

// CPU reference histogram.
void histogramCPU(const int* data, int n, int* hist, int numBins) {
    for (int i = 0; i < numBins; ++i) hist[i] = 0;
    for (int i = 0; i < n; ++i) {
        if (data[i] >= 0 && data[i] < numBins)
            hist[data[i]]++;
    }
}

// ============================================================================
// (a) Naive: global atomics.
// Every thread does atomicAdd(&hist[data[i]], 1) directly on global memory.
// With skewed data, many threads hammer the same bin → serialisation.
// ============================================================================
__global__ void histogramNaive(const int* __restrict__ data,
                               int n,
                               int* __restrict__ hist,
                               int numBins) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        int bin = data[i];
        if (bin >= 0 && bin < numBins)
            atomicAdd(&hist[bin], 1);
    }
}

// ============================================================================
// (b) Shared-memory privatized histogram.
// Each block has its own shared-memory copy of the histogram.  Threads
// atomicAdd to shared (much less contention since only threads in the same
// block compete).  At the end, block threads cooperatively merge their
// private histogram into the global one.
//
// Requires: numBins * sizeof(int) <= shared memory per block (~48 KB).
// ============================================================================
__global__ void histogramShared(const int* __restrict__ data,
                                int n,
                                int* __restrict__ hist,
                                int numBins) {
    extern __shared__ int s_hist[];

    // Initialize shared histogram to 0.
    for (int i = threadIdx.x; i < numBins; i += blockDim.x) {
        s_hist[i] = 0;
    }
    __syncthreads();

    // Accumulate into shared histogram.
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        int bin = data[i];
        if (bin >= 0 && bin < numBins)
            atomicAdd(&s_hist[bin], 1);
    }
    __syncthreads();

    // Merge shared → global.
    for (int b = threadIdx.x; b < numBins; b += blockDim.x) {
        if (s_hist[b] > 0)
            atomicAdd(&hist[b], s_hist[b]);
    }
}

// ============================================================================
// (c) Large-bin histogram: when numBins doesn't fit in shared memory,
// we process a subset of bins at a time ("tiled" approach).
// Each pass handles bins [lo, hi) using shared memory.
// ============================================================================
__global__ void histogramLargeBins(const int* __restrict__ data,
                                   int n,
                                   int* __restrict__ hist,
                                   int numBins,
                                   int binLo, int binHi) {
    int range = binHi - binLo;
    extern __shared__ int s_hist[];

    for (int i = threadIdx.x; i < range; i += blockDim.x)
        s_hist[i] = 0;
    __syncthreads();

    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        int bin = data[i];
        if (bin >= binLo && bin < binHi)
            atomicAdd(&s_hist[bin - binLo], 1);
    }
    __syncthreads();

    for (int b = threadIdx.x; b < range; b += blockDim.x) {
        if (s_hist[b] > 0)
            atomicAdd(&hist[b + binLo], s_hist[b]);
    }
}

void runLargeBinHistogram(const int* d_data, int n, int* d_hist, int numBins) {
    int maxSmemInts = 12288;  // 48 KB / 4 bytes
    int blocks = (n + TPB - 1) / TPB;

    for (int lo = 0; lo < numBins; lo += maxSmemInts) {
        int hi = std::min(lo + maxSmemInts, numBins);
        int range = hi - lo;
        histogramLargeBins<<<blocks, TPB, range * sizeof(int)>>>(
            d_data, n, d_hist, numBins, lo, hi);
        CUDA_CHECK_KERNEL();
    }
}

// ============================================================================
int main() {
    printf("\n===== RUNG 6: Histogram =====\n\n");

    DeviceInfo info = queryDevice();

    const int N = 1 << 24;
    const int NUM_BINS = 256;
    const size_t dataBytes = N * sizeof(int);
    const size_t histBytes = NUM_BINS * sizeof(int);
    const int blocks = (N + TPB - 1) / TPB;

    int* h_data  = (int*)malloc(dataBytes);
    int* h_hist  = (int*)malloc(histBytes);
    int* h_ref   = (int*)malloc(histBytes);

    int* d_data;
    int* d_hist;
    CUDA_CHECK(cudaMalloc(&d_data, dataBytes));
    CUDA_CHECK(cudaMalloc(&d_hist, histBytes));

    std::mt19937 rng(42);

    // ---- Test with UNIFORM distribution ----
    auto runTest = [&](const char* distName, auto genFn) {
        for (int i = 0; i < N; ++i) h_data[i] = genFn(rng);
        histogramCPU(h_data, N, h_ref, NUM_BINS);
        CUDA_CHECK(cudaMemcpy(d_data, h_data, dataBytes, cudaMemcpyHostToDevice));

        printf("  Distribution: %s\n", distName);
        printf("  %-40s %8s  %8s\n", "Variant", "ms", "GB/s");
        printf("  %-40s %8s  %8s\n", "-------", "--", "----");

        // (a) Naive.
        CUDA_CHECK(cudaMemset(d_hist, 0, histBytes));
        histogramNaive<<<blocks, TPB>>>(d_data, N, d_hist, NUM_BINS);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_hist, d_hist, histBytes, cudaMemcpyDeviceToHost));
        if (!verifyInts(h_ref, h_hist, NUM_BINS)) {
            printf("  FAIL: naive\n"); return;
        }

        BenchResult r1 = benchmark([&](GpuTimer& t) {
            CUDA_CHECK(cudaMemset(d_hist, 0, histBytes));
            t.start();
            histogramNaive<<<blocks, TPB>>>(d_data, N, d_hist, NUM_BINS);
            t.stop();
        });
        double gbps1 = dataBytes / (r1.medianMs / 1000.0) / 1e9;
        printf("  %-40s %8.3f  %8.1f\n", "naive (global atomics)", r1.medianMs, gbps1);

        // (b) Shared memory.
        CUDA_CHECK(cudaMemset(d_hist, 0, histBytes));
        histogramShared<<<blocks, TPB, NUM_BINS * sizeof(int)>>>(d_data, N, d_hist, NUM_BINS);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_hist, d_hist, histBytes, cudaMemcpyDeviceToHost));
        if (!verifyInts(h_ref, h_hist, NUM_BINS)) {
            printf("  FAIL: shared\n"); return;
        }

        BenchResult r2 = benchmark([&](GpuTimer& t) {
            CUDA_CHECK(cudaMemset(d_hist, 0, histBytes));
            t.start();
            histogramShared<<<blocks, TPB, NUM_BINS * sizeof(int)>>>(d_data, N, d_hist, NUM_BINS);
            t.stop();
        });
        double gbps2 = dataBytes / (r2.medianMs / 1000.0) / 1e9;
        printf("  %-40s %8.3f  %8.1f\n", "shared-mem privatized", r2.medianMs, gbps2);

        // (c) Large-bin (same bins but exercises the tiled path).
        CUDA_CHECK(cudaMemset(d_hist, 0, histBytes));
        runLargeBinHistogram(d_data, N, d_hist, NUM_BINS);
        CUDA_CHECK(cudaMemcpy(h_hist, d_hist, histBytes, cudaMemcpyDeviceToHost));
        if (!verifyInts(h_ref, h_hist, NUM_BINS)) {
            printf("  FAIL: large-bin\n"); return;
        }
        printf("  %-40s %8s  %8s\n", "large-bin tiled", "(see above)", "n/a");

        printf("  Speedup (shared/naive): %.2fx\n\n", r1.medianMs / r2.medianMs);
    };

    // Uniform distribution.
    std::uniform_int_distribution<int> uniformDist(0, NUM_BINS - 1);
    runTest("UNIFORM", [&](std::mt19937& r) { return uniformDist(r); });

    // Skewed distribution: 90% of values in bins 0-3.
    runTest("SKEWED (90% in 4 bins)", [&](std::mt19937& r) -> int {
        float u = std::uniform_real_distribution<float>(0.0f, 1.0f)(r);
        if (u < 0.9f) return (int)(u / 0.9f * 4);  // bins 0-3
        return (int)((u - 0.9f) / 0.1f * (NUM_BINS - 4)) + 4;
    });

    printf("  KEY TAKEAWAY: With skewed data, naive global atomics are a disaster.\n");
    printf("  Privatization in shared memory reduces contention dramatically.\n\n");

    CUDA_CHECK(cudaFree(d_data));
    CUDA_CHECK(cudaFree(d_hist));
    free(h_data); free(h_hist); free(h_ref);

    return 0;
}
