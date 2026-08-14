// 08_radix_sort.cu — Rung 8: LSD Radix Sort
// ============================================================================
// LESSON: Composing primitives (histogram, scan, scatter) into a complete
// algorithm.  This rung is where the previous rungs pay off.
//
// We implement a least-significant-digit (LSD) radix sort on 32-bit unsigned
// integers, processing 4 bits (one hex digit) at a time = 16 buckets.
// For each digit position:
//   1. Compute a per-block histogram of the 4-bit digit (16 bins per block).
//   2. Prefix-sum (scan) the histograms to get global scatter offsets.
//   3. Scatter elements to their sorted positions.
//   4. Swap input/output buffers and repeat for the next digit.
//
// 8 passes (32 bits / 4 bits) produce a fully sorted array.
//
// This is a CORRECTNESS-first implementation.  Production radix sorts (CUB)
// are far more optimized, but the structure is the same.
//
// INTERVIEW: "How does radix sort map to GPU parallelism?"
//            "What primitives compose a GPU sort?"
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include "common/helpers.cuh"

static const int TPB = 256;
static const int RADIX_BITS = 4;
static const int RADIX = 1 << RADIX_BITS;  // 16 buckets

// ============================================================================
// Kernel 1: Per-block histogram of the current 4-bit digit.
// Each block processes blockDim.x elements and produces a 16-bin histogram.
// Output: hist[block * RADIX + digit] = count.
// ============================================================================
__global__ void radixHistogram(const unsigned int* __restrict__ keys,
                               unsigned int* __restrict__ hist,
                               int n, int shift) {
    __shared__ unsigned int s_hist[RADIX];

    if (threadIdx.x < RADIX) s_hist[threadIdx.x] = 0;
    __syncthreads();

    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        unsigned int digit = (keys[i] >> shift) & (RADIX - 1);
        atomicAdd(&s_hist[digit], 1);
    }
    __syncthreads();

    if (threadIdx.x < RADIX) {
        hist[blockIdx.x * RADIX + threadIdx.x] = s_hist[threadIdx.x];
    }
}

// ============================================================================
// Kernel 2: Exclusive prefix sum on the histogram array.
// The histogram has numBlocks * RADIX elements, laid out as:
//   [block0_digit0, block0_digit1, ..., block0_digit15,
//    block1_digit0, ..., blockN_digit15]
// We scan this in column-major order (all blocks' digit-0, then digit-1, etc.)
// to get global offsets.
//
// For simplicity, we do this on CPU.  A production implementation would use
// a GPU scan.
// ============================================================================
void exclusiveScanCPU(unsigned int* data, int n) {
    unsigned int sum = 0;
    for (int i = 0; i < n; ++i) {
        unsigned int val = data[i];
        data[i] = sum;
        sum += val;
    }
}

// ============================================================================
// Kernel 3: Scatter elements to sorted positions based on scanned offsets.
// Each thread computes its digit, looks up the global offset in the scanned
// histogram, and uses atomicAdd on shared memory to get a unique position.
// ============================================================================
__global__ void radixScatter(const unsigned int* __restrict__ keysIn,
                             unsigned int* __restrict__ keysOut,
                             const unsigned int* __restrict__ prefixes,
                             int n, int shift) {
    __shared__ unsigned int s_offsets[RADIX];

    // Load this block's prefix for each digit.
    if (threadIdx.x < RADIX) {
        s_offsets[threadIdx.x] = prefixes[blockIdx.x * RADIX + threadIdx.x];
    }
    __syncthreads();

    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        unsigned int key = keysIn[i];
        unsigned int digit = (key >> shift) & (RADIX - 1);
        unsigned int pos = atomicAdd(&s_offsets[digit], 1);
        keysOut[pos] = key;
    }
}

// ============================================================================
// Host-side radix sort.
// ============================================================================
void radixSort(unsigned int* d_keysIn, unsigned int* d_keysOut,
               unsigned int* d_hist, unsigned int* h_hist,
               int n) {
    int numBlocks = (n + TPB - 1) / TPB;
    int histSize = numBlocks * RADIX;

    for (int shift = 0; shift < 32; shift += RADIX_BITS) {
        // 1. Histogram.
        CUDA_CHECK(cudaMemset(d_hist, 0, histSize * sizeof(unsigned int)));
        radixHistogram<<<numBlocks, TPB>>>(d_keysIn, d_hist, n, shift);
        CUDA_CHECK_KERNEL();

        // 2. Scan on CPU (column-major order).
        // We need to scan in digit-major order: for each digit d, scan across blocks.
        CUDA_CHECK(cudaMemcpy(h_hist, d_hist, histSize * sizeof(unsigned int),
                              cudaMemcpyDeviceToHost));

        // Transpose to column-major, scan, transpose back.
        unsigned int* colMajor = (unsigned int*)malloc(histSize * sizeof(unsigned int));
        for (int b = 0; b < numBlocks; ++b)
            for (int d = 0; d < RADIX; ++d)
                colMajor[d * numBlocks + b] = h_hist[b * RADIX + d];

        exclusiveScanCPU(colMajor, histSize);

        for (int b = 0; b < numBlocks; ++b)
            for (int d = 0; d < RADIX; ++d)
                h_hist[b * RADIX + d] = colMajor[d * numBlocks + b];

        free(colMajor);

        CUDA_CHECK(cudaMemcpy(d_hist, h_hist, histSize * sizeof(unsigned int),
                              cudaMemcpyHostToDevice));

        // 3. Scatter.
        radixScatter<<<numBlocks, TPB>>>(d_keysIn, d_keysOut, d_hist, n, shift);
        CUDA_CHECK_KERNEL();

        // 4. Swap.
        unsigned int* temp = d_keysIn;
        d_keysIn = d_keysOut;
        d_keysOut = temp;
    }

    // After 8 passes (32/4), the result is in d_keysIn (because of swaps).
    // If we started with the original d_keysIn, after an even number of swaps
    // it's back in the original buffer.  32/4 = 8 swaps = even → result in
    // the original d_keysIn.  But the caller passed the original d_keysIn as
    // the first arg, so we need to check.  For simplicity, copy if needed.
    // (8 swaps means d_keysIn now points to the ORIGINAL d_keysIn → correct.)
}

int main() {
    printf("\n===== RUNG 8: Radix Sort =====\n\n");

    DeviceInfo info = queryDevice();

    const int N = 1 << 20;  // 1M elements (keep moderate for correctness focus)
    const size_t bytes = N * sizeof(unsigned int);

    unsigned int* h_keys = (unsigned int*)malloc(bytes);
    unsigned int* h_sorted = (unsigned int*)malloc(bytes);
    unsigned int* h_ref = (unsigned int*)malloc(bytes);

    // Random data.
    std::mt19937 rng(42);
    for (int i = 0; i < N; ++i) h_keys[i] = rng();

    // CPU reference sort.
    memcpy(h_ref, h_keys, bytes);
    std::sort(h_ref, h_ref + N);

    int numBlocks = (N + TPB - 1) / TPB;
    int histSize = numBlocks * RADIX;

    unsigned int *d_keysA, *d_keysB, *d_hist;
    unsigned int* h_hist = (unsigned int*)malloc(histSize * sizeof(unsigned int));
    CUDA_CHECK(cudaMalloc(&d_keysA, bytes));
    CUDA_CHECK(cudaMalloc(&d_keysB, bytes));
    CUDA_CHECK(cudaMalloc(&d_hist, histSize * sizeof(unsigned int)));
    CUDA_CHECK(cudaMemcpy(d_keysA, h_keys, bytes, cudaMemcpyHostToDevice));

    // Sort.
    radixSort(d_keysA, d_keysB, d_hist, h_hist, N);

    // Result is in d_keysA after 8 swaps (even).
    CUDA_CHECK(cudaMemcpy(h_sorted, d_keysA, bytes, cudaMemcpyDeviceToHost));

    // Verify.
    bool ok = true;
    for (int i = 0; i < N; ++i) {
        if (h_sorted[i] != h_ref[i]) {
            printf("  MISMATCH at index %d: got %u, expected %u\n",
                   i, h_sorted[i], h_ref[i]);
            ok = false;
            break;
        }
    }
    printf("  Radix sort (N=%d): %s\n\n", N, ok ? "PASSED" : "FAILED");

    // Benchmark.
    if (ok) {
        BenchResult res = benchmark([&](GpuTimer& t) {
            CUDA_CHECK(cudaMemcpy(d_keysA, h_keys, bytes, cudaMemcpyHostToDevice));
            t.start();
            radixSort(d_keysA, d_keysB, d_hist, h_hist, N);
            t.stop();
        });
        // Bandwidth: each pass reads + writes N keys = 2N × 4 bytes, 8 passes.
        double totalBytes = 2.0 * bytes * (32.0 / RADIX_BITS);
        reportBandwidth("radix sort (8 passes)", res.medianMs,
                        totalBytes, info.peakBandwidthGBs);
    }

    printf("  This rung composes histogram + scan + scatter — the building blocks\n");
    printf("  from previous rungs.  Production sorts (CUB) add many optimizations:\n");
    printf("  larger radix, local sorting within blocks, decoupled look-back scan.\n\n");

    CUDA_CHECK(cudaFree(d_keysA));
    CUDA_CHECK(cudaFree(d_keysB));
    CUDA_CHECK(cudaFree(d_hist));
    free(h_keys); free(h_sorted); free(h_ref); free(h_hist);

    return 0;
}
