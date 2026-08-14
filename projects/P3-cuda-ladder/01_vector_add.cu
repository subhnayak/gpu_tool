// 01_vector_add.cu — Rung 1: Vector Addition
// ============================================================================
// LESSON: Vector addition is memory-bound. The arithmetic (one ADD per element)
// is essentially free — performance is determined entirely by how fast we can
// stream data through the memory system.  This rung establishes the measurement
// methodology used throughout the ladder.
//
// We implement two variants:
//   (a) One-thread-per-element: exactly N threads, each does one load-load-store.
//   (b) Grid-stride loop: fewer threads, each processes multiple elements.
//       This is the PREFERRED idiom in production code because the grid size
//       is decoupled from the problem size.
//
// INTERVIEW QUESTION: "What determines the performance of a simple vector-add
// kernel?"  Answer: memory bandwidth.  The kernel performs 3 memory transactions
// per element (2 loads + 1 store) × 4 bytes = 12 bytes/element and only 1 FLOP.
// Arithmetic intensity = 1/12 ≈ 0.08 FLOP/byte — far below any GPU's ridge
// point on the roofline.
//
// EXPECTED OBSERVATION: Both variants should achieve ~80-90% of peak memory
// bandwidth on a modern GPU.  The grid-stride loop may be marginally slower
// on very old GPUs due to fewer threads, but on modern GPUs (many SMs) the
// difference is negligible.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "common/helpers.cuh"

// ----------------------------------------------------------------------------
// Kernel (a): one thread per element — simplest possible kernel.
// Thread i computes C[i] = A[i] + B[i].
// No boundary check needed if we launch exactly N threads, but we add one
// for safety (and so the pattern is clear for non-power-of-2 sizes).
// ----------------------------------------------------------------------------
__global__ void vectorAddSimple(const float* __restrict__ A,
                                const float* __restrict__ B,
                                float* __restrict__ C,
                                int N) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N) {
        C[i] = A[i] + B[i];
    }
}

// ----------------------------------------------------------------------------
// Kernel (b): grid-stride loop.
// Each thread walks through the array with a stride equal to the total number
// of threads in the grid.  This pattern is:
//   - Reusable for any N without changing the launch config.
//   - Better for very large N: the grid size can stay fixed while N grows.
//   - Identical performance for moderate N.
//
// Interview tie-in: "Why use a grid-stride loop?"
// Answer: It decouples problem size from grid dimensions, handles arbitrary N,
// and amortises launch overhead.
// ----------------------------------------------------------------------------
__global__ void vectorAddGridStride(const float* __restrict__ A,
                                    const float* __restrict__ B,
                                    float* __restrict__ C,
                                    int N) {
    int stride = blockDim.x * gridDim.x;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < N; i += stride) {
        C[i] = A[i] + B[i];
    }
}

// ----------------------------------------------------------------------------
// CPU reference for verification.
// ----------------------------------------------------------------------------
void vectorAddCPU(const float* A, const float* B, float* C, int N) {
    for (int i = 0; i < N; ++i) C[i] = A[i] + B[i];
}

// ============================================================================
int main() {
    printf("\n===== RUNG 1: Vector Addition =====\n\n");

    DeviceInfo info = queryDevice();

    // Problem size — large enough to saturate bandwidth.
    const int N = 1 << 24;  // 16 M elements = 64 MB per array
    const size_t bytes = N * sizeof(float);

    // Total memory traffic: 2 loads + 1 store = 3 arrays.
    const double totalBytes = 3.0 * bytes;

    // Host allocations.
    float* h_A = (float*)malloc(bytes);
    float* h_B = (float*)malloc(bytes);
    float* h_C = (float*)malloc(bytes);
    float* h_ref = (float*)malloc(bytes);

    // Initialise with reproducible data.
    for (int i = 0; i < N; ++i) {
        h_A[i] = sinf(i * 0.001f);
        h_B[i] = cosf(i * 0.001f);
    }

    // CPU reference.
    vectorAddCPU(h_A, h_B, h_ref, N);

    // Device allocations.
    float *d_A, *d_B, *d_C;
    CUDA_CHECK(cudaMalloc(&d_A, bytes));
    CUDA_CHECK(cudaMalloc(&d_B, bytes));
    CUDA_CHECK(cudaMalloc(&d_C, bytes));
    CUDA_CHECK(cudaMemcpy(d_A, h_A, bytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_B, h_B, bytes, cudaMemcpyHostToDevice));

    // ---- Variant (a): one-thread-per-element ----
    {
        const int threadsPerBlock = 256;
        const int blocks = (N + threadsPerBlock - 1) / threadsPerBlock;

        // Verify correctness first.
        vectorAddSimple<<<blocks, threadsPerBlock>>>(d_A, d_B, d_C, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        if (!verifyFloats(h_ref, h_C, N)) {
            printf("  FAIL: vectorAddSimple produced incorrect results!\n");
            return 1;
        }
        printf("  vectorAddSimple: PASSED verification\n");

        // Benchmark.
        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            vectorAddSimple<<<blocks, threadsPerBlock>>>(d_A, d_B, d_C, N);
            t.stop();
        });
        reportBandwidth("vectorAddSimple", res.medianMs,
                        totalBytes, info.peakBandwidthGBs);
    }

    // ---- Variant (b): grid-stride loop ----
    {
        // Intentionally use fewer blocks to show the grid-stride pattern.
        const int threadsPerBlock = 256;
        const int blocks = 256;  // much fewer than needed for 1-per-element

        vectorAddGridStride<<<blocks, threadsPerBlock>>>(d_A, d_B, d_C, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        if (!verifyFloats(h_ref, h_C, N)) {
            printf("  FAIL: vectorAddGridStride produced incorrect results!\n");
            return 1;
        }
        printf("  vectorAddGridStride: PASSED verification\n");

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            vectorAddGridStride<<<blocks, threadsPerBlock>>>(d_A, d_B, d_C, N);
            t.stop();
        });
        reportBandwidth("vectorAddGridStride", res.medianMs,
                        totalBytes, info.peakBandwidthGBs);
    }

    printf("\n  NOTE: Both variants should achieve similar bandwidth.\n");
    printf("  This kernel is MEMORY-BOUND. The single ADD per element is free.\n\n");

    // Cleanup.
    CUDA_CHECK(cudaFree(d_A));
    CUDA_CHECK(cudaFree(d_B));
    CUDA_CHECK(cudaFree(d_C));
    free(h_A); free(h_B); free(h_C); free(h_ref);

    return 0;
}
