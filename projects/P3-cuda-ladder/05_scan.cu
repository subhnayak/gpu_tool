// 05_scan.cu — Rung 5: Prefix Sum (Scan)
// ============================================================================
// LESSON: Scan is a fundamental building block (used in sort, compact, etc.).
//
// We implement:
//   (a) Hillis-Steele scan — work-inefficient O(n log n), but simple.
//   (b) Blelloch (work-efficient) scan — O(n), up-sweep + down-sweep.
//   (c) Multi-block scan: block-level scans + scan of block sums + propagate.
//
// All produce an EXCLUSIVE prefix sum for easy verification.
//
// INTERVIEW: "Implement a parallel prefix sum."
//            "What is the decoupled look-back algorithm?"
//
// DECOUPLED LOOK-BACK (comment only): CUB/Thrust use the Merrill-Garland
// decoupled look-back, where each block publishes its local aggregate, then
// looks back at preceding blocks in a chain.  This avoids the two-pass
// approach and achieves near-peak bandwidth.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "common/helpers.cuh"

static const int TPB = 256;

// CPU exclusive scan.
void exclusiveScanCPU(const float* in, float* out, int n) {
    out[0] = 0.0f;
    for (int i = 1; i < n; ++i) out[i] = out[i - 1] + in[i - 1];
}

// ============================================================================
// (a) Hillis-Steele inclusive scan (single block).
// Work = O(n log n).  Step count = O(log n).
// We convert to exclusive by shifting right and inserting 0.
// ============================================================================
__global__ void hillisSteeleScan(const float* __restrict__ in,
                                 float* __restrict__ out, int n) {
    extern __shared__ float temp[];
    int tid = threadIdx.x;
    if (tid < n) temp[tid] = in[tid]; else temp[tid] = 0.0f;
    __syncthreads();

    for (int offset = 1; offset < n; offset <<= 1) {
        float val = 0.0f;
        if (tid >= offset) val = temp[tid - offset];
        __syncthreads();
        if (tid >= offset) temp[tid] += val;
        __syncthreads();
    }

    // Convert inclusive → exclusive: shift right, insert 0.
    if (tid < n) {
        out[tid] = (tid > 0) ? temp[tid - 1] : 0.0f;
    }
}

// ============================================================================
// (b) Blelloch work-efficient scan (single block).
// Up-sweep: build partial sums in a tree.
// Down-sweep: traverse back to produce the prefix sum.
// Work = O(n), Steps = O(2 log n).
// ============================================================================
__global__ void blellochScan(const float* __restrict__ in,
                             float* __restrict__ out, int n) {
    extern __shared__ float temp[];
    int tid = threadIdx.x;
    // Load — note: shared memory must be allocated for pow2 elements, not n.
    int pow2 = 1;
    while (pow2 < n) pow2 <<= 1;

    if (2 * tid < n)     temp[2 * tid]     = in[2 * tid];     else if (2 * tid < pow2)     temp[2 * tid]     = 0.0f;
    if (2 * tid + 1 < n) temp[2 * tid + 1] = in[2 * tid + 1]; else if (2 * tid + 1 < pow2) temp[2 * tid + 1] = 0.0f;
    __syncthreads();

    // Up-sweep (reduce).
    for (int stride = 1; stride < pow2; stride <<= 1) {
        int index = (2 * stride) * (tid + 1) - 1;
        if (index < pow2) {
            temp[index] += temp[index - stride];
        }
        __syncthreads();
    }

    // Set root to 0 (exclusive scan).
    if (tid == 0) temp[pow2 - 1] = 0.0f;
    __syncthreads();

    // Down-sweep.
    for (int stride = pow2 / 2; stride > 0; stride >>= 1) {
        int index = (2 * stride) * (tid + 1) - 1;
        if (index < pow2) {
            float t = temp[index - stride];
            temp[index - stride] = temp[index];
            temp[index] += t;
        }
        __syncthreads();
    }

    // Write output.
    if (2 * tid < n)     out[2 * tid]     = temp[2 * tid];
    if (2 * tid + 1 < n) out[2 * tid + 1] = temp[2 * tid + 1];
}

// ============================================================================
// (c) Multi-block scan.
// Phase 1: Each block scans its chunk and writes its total to blockSums[].
// Phase 2: Scan blockSums[] (single block since we limit to 256*2*256 = 128K).
// Phase 3: Add the scanned blockSum to each element in the block's output.
//
// For larger arrays, this generalizes to a recursive multi-level scan.
// Production libraries (CUB) use decoupled look-back instead.
// ============================================================================

// Phase 1: Blelloch scan within each block, save block total.
__global__ void scanPhase1(const float* __restrict__ in,
                           float* __restrict__ out,
                           float* __restrict__ blockSums,
                           int n) {
    extern __shared__ float temp[];
    int tid = threadIdx.x;
    int blockOffset = blockIdx.x * (blockDim.x * 2);
    int gid0 = blockOffset + 2 * tid;
    int gid1 = blockOffset + 2 * tid + 1;

    temp[2 * tid]     = (gid0 < n) ? in[gid0] : 0.0f;
    temp[2 * tid + 1] = (gid1 < n) ? in[gid1] : 0.0f;
    __syncthreads();

    int chunkSize = blockDim.x * 2;
    int pow2 = 1;
    while (pow2 < chunkSize) pow2 <<= 1;

    // Up-sweep.
    for (int s = 1; s < pow2; s <<= 1) {
        int idx = (2 * s) * (tid + 1) - 1;
        if (idx < pow2) temp[idx] += temp[idx - s];
        __syncthreads();
    }

    // Save block total, set root to 0.
    if (tid == 0) {
        blockSums[blockIdx.x] = temp[pow2 - 1];
        temp[pow2 - 1] = 0.0f;
    }
    __syncthreads();

    // Down-sweep.
    for (int s = pow2 / 2; s > 0; s >>= 1) {
        int idx = (2 * s) * (tid + 1) - 1;
        if (idx < pow2) {
            float t = temp[idx - s];
            temp[idx - s] = temp[idx];
            temp[idx] += t;
        }
        __syncthreads();
    }

    if (gid0 < n) out[gid0] = temp[2 * tid];
    if (gid1 < n) out[gid1] = temp[2 * tid + 1];
}

// Phase 3: Add scanned block sums to each element.
__global__ void scanPhase3(float* __restrict__ data,
                           const float* __restrict__ blockSums,
                           int n) {
    int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;
    float add = blockSums[blockIdx.x];
    if (i < n) data[i] += add;
    if (i + blockDim.x < n) data[i + blockDim.x] += add;
}

// Full multi-block scan.
void multiBlockScan(const float* d_in, float* d_out, int n, float peakGBs) {
    int elemsPerBlock = TPB * 2;
    int numBlocks = (n + elemsPerBlock - 1) / elemsPerBlock;

    float* d_blockSums;
    float* d_blockSumsScanned;
    CUDA_CHECK(cudaMalloc(&d_blockSums, numBlocks * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_blockSumsScanned, numBlocks * sizeof(float)));

    size_t smem = elemsPerBlock * sizeof(float);

    // Phase 1: per-block scan.
    scanPhase1<<<numBlocks, TPB, smem>>>(d_in, d_out, d_blockSums, n);
    CUDA_CHECK_KERNEL();

    // Phase 2: scan block sums (single block — ok if numBlocks <= 512).
    if (numBlocks <= 1) {
        CUDA_CHECK(cudaMemset(d_blockSumsScanned, 0, sizeof(float)));
    } else {
        // Allocate pow2-rounded shared memory for blellochScan.
        int pow2Blocks = 1;
        while (pow2Blocks < numBlocks) pow2Blocks <<= 1;
        blellochScan<<<1, (numBlocks + 1) / 2, pow2Blocks * sizeof(float)>>>(
            d_blockSums, d_blockSumsScanned, numBlocks);
        CUDA_CHECK_KERNEL();
    }

    // Phase 3: propagate.
    if (numBlocks > 1) {
        scanPhase3<<<numBlocks, TPB>>>(d_out, d_blockSumsScanned, n);
        CUDA_CHECK_KERNEL();
    }

    CUDA_CHECK(cudaFree(d_blockSums));
    CUDA_CHECK(cudaFree(d_blockSumsScanned));
}

int main() {
    printf("\n===== RUNG 5: Prefix Sum (Scan) =====\n\n");

    DeviceInfo info = queryDevice();

    // ---- Single-block tests (small N) ----
    {
        const int N_SMALL = 512;  // fits in one block
        size_t bytes = N_SMALL * sizeof(float);
        float* h_in  = (float*)malloc(bytes);
        float* h_out = (float*)malloc(bytes);
        float* h_ref = (float*)malloc(bytes);
        for (int i = 0; i < N_SMALL; ++i) h_in[i] = 1.0f;
        exclusiveScanCPU(h_in, h_ref, N_SMALL);

        float *d_in, *d_out;
        CUDA_CHECK(cudaMalloc(&d_in, bytes));
        CUDA_CHECK(cudaMalloc(&d_out, bytes));
        CUDA_CHECK(cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice));

        // Hillis-Steele.
        CUDA_CHECK(cudaMemset(d_out, 0, bytes));
        hillisSteeleScan<<<1, N_SMALL, N_SMALL * sizeof(float)>>>(d_in, d_out, N_SMALL);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));
        printf("  Hillis-Steele (single block, N=%d): %s\n",
               N_SMALL, verifyFloats(h_ref, h_out, N_SMALL, 1e-2f, 1e-4f) ? "PASSED" : "FAILED");

        // Blelloch — must allocate pow2 elements of shared memory.
        int pow2_small = 1;
        while (pow2_small < N_SMALL) pow2_small <<= 1;
        CUDA_CHECK(cudaMemset(d_out, 0, bytes));
        blellochScan<<<1, N_SMALL / 2, pow2_small * sizeof(float)>>>(d_in, d_out, N_SMALL);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));
        printf("  Blelloch (single block, N=%d):      %s\n",
               N_SMALL, verifyFloats(h_ref, h_out, N_SMALL, 1e-2f, 1e-4f) ? "PASSED" : "FAILED");

        CUDA_CHECK(cudaFree(d_in));
        CUDA_CHECK(cudaFree(d_out));
        free(h_in); free(h_out); free(h_ref);
    }

    // ---- Multi-block test (large N) ----
    {
        const int N_LARGE = 1 << 18;  // 256K elements
        size_t bytes = N_LARGE * sizeof(float);
        double totalBytes = 2.0 * bytes;

        float* h_in  = (float*)malloc(bytes);
        float* h_out = (float*)malloc(bytes);
        float* h_ref = (float*)malloc(bytes);
        for (int i = 0; i < N_LARGE; ++i) h_in[i] = 1.0f;
        exclusiveScanCPU(h_in, h_ref, N_LARGE);

        float *d_in, *d_out;
        CUDA_CHECK(cudaMalloc(&d_in, bytes));
        CUDA_CHECK(cudaMalloc(&d_out, bytes));
        CUDA_CHECK(cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice));

        multiBlockScan(d_in, d_out, N_LARGE, info.peakBandwidthGBs);
        CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));
        bool ok = verifyFloats(h_ref, h_out, N_LARGE, 1.0f, 1e-4f);
        printf("  Multi-block scan (N=%d):          %s\n\n", N_LARGE, ok ? "PASSED" : "FAILED");

        if (ok) {
            BenchResult res = benchmark([&](GpuTimer& t) {
                t.start();
                multiBlockScan(d_in, d_out, N_LARGE, info.peakBandwidthGBs);
                t.stop();
            });
            reportBandwidth("multi-block scan", res.medianMs,
                            totalBytes, info.peakBandwidthGBs);
        }

        CUDA_CHECK(cudaFree(d_in));
        CUDA_CHECK(cudaFree(d_out));
        free(h_in); free(h_out); free(h_ref);
    }

    printf("\n  STATE-OF-THE-ART: Decoupled look-back (Merrill & Garland, 2016)\n");
    printf("  avoids the two-pass approach.  Each block publishes its aggregate\n");
    printf("  and looks back at predecessors in a chain.  CUB implements this.\n\n");

    return 0;
}
