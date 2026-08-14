// 07_sgemm.cu — Rung 7: Matrix Multiply (SGEMM)
// ============================================================================
// THE canonical compute-bound kernel.  C = A × B, all float, all square.
//
// Arithmetic intensity = 2N³ FLOPs / (3N² × 4 bytes) = N/6 FLOP/byte.
// For N ≥ 1024 this is well above the roofline ridge point → compute-bound.
//
// We implement:
//   (a) Naive: one thread per output element, each does N MADs.
//   (b) Shared-memory tiled (TILE=32): load tiles of A and B into shared,
//       compute partial products, iterate over tiles.
//   (c) Register-tiled / thread-coarsened: each thread computes a TM×TN
//       micro-tile (4×4) using registers, dramatically reducing smem traffic.
//   (d) Optional cuBLAS comparison (guarded by USE_CUBLAS).
//
// INTERVIEW: "How would you optimize matrix multiply on a GPU?"
//            "What is arithmetic intensity and the roofline model?"
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "common/helpers.cuh"

#ifdef USE_CUBLAS
#include <cublas_v2.h>
#endif

// Matrix dimension (square).  Must be divisible by 32 for the tiled kernels.
static const int M_DIM = 1024;

// ============================================================================
// CPU reference.
// ============================================================================
void sgemmCPU(const float* A, const float* B, float* C, int N) {
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < N; ++k)
                sum += A[i * N + k] * B[k * N + j];
            C[i * N + j] = sum;
        }
}

// ============================================================================
// (a) Naive: one thread per output element.
// Thread (row, col) computes C[row][col] = dot(A[row][:], B[:][col]).
// No data reuse — each element of A and B is loaded N times total.
// ============================================================================
__global__ void sgemmNaive(const float* __restrict__ A,
                           const float* __restrict__ B,
                           float* __restrict__ C,
                           int N) {
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    int col = blockIdx.x * blockDim.x + threadIdx.x;
    if (row < N && col < N) {
        float sum = 0.0f;
        for (int k = 0; k < N; ++k)
            sum += A[row * N + k] * B[k * N + col];
        C[row * N + col] = sum;
    }
}

// ============================================================================
// (b) Shared-memory tiled (TILE=32).
// Each block computes a 32×32 tile of C.  For each "k-step," it loads a
// 32×32 tile of A and 32×32 tile of B into shared memory, then each thread
// accumulates 32 MADs.  This reduces global memory traffic by TILE×.
// ============================================================================
static const int TILE = 32;

__global__ void sgemmTiled(const float* __restrict__ A,
                           const float* __restrict__ B,
                           float* __restrict__ C,
                           int N) {
    __shared__ float As[TILE][TILE];
    __shared__ float Bs[TILE][TILE];

    int row = blockIdx.y * TILE + threadIdx.y;
    int col = blockIdx.x * TILE + threadIdx.x;
    float sum = 0.0f;

    for (int t = 0; t < N; t += TILE) {
        // Cooperative load: each thread loads one element of each tile.
        if (row < N && (t + threadIdx.x) < N)
            As[threadIdx.y][threadIdx.x] = A[row * N + t + threadIdx.x];
        else
            As[threadIdx.y][threadIdx.x] = 0.0f;

        if (col < N && (t + threadIdx.y) < N)
            Bs[threadIdx.y][threadIdx.x] = B[(t + threadIdx.y) * N + col];
        else
            Bs[threadIdx.y][threadIdx.x] = 0.0f;

        __syncthreads();  // Tile fully loaded.

        for (int k = 0; k < TILE; ++k)
            sum += As[threadIdx.y][k] * Bs[k][threadIdx.x];

        __syncthreads();  // Before overwriting tile in next iteration.
    }

    if (row < N && col < N)
        C[row * N + col] = sum;
}

// ============================================================================
// (c) Register-tiled / thread-coarsened (TM×TN micro-tile per thread).
// Each thread computes a 4×4 block of C using registers.  The block of
// threads cooperatively loads tiles of A and B, but each thread accumulates
// into TM×TN register accumulators.  This increases arithmetic per shared-
// memory load, improving compute utilization.
//
// Block dims: (TILE/TN, TILE/TM) = (8, 8) threads, each computing 4×4 output.
// Block output tile: TILE × TILE = 32 × 32 elements.
// ============================================================================
static const int TM = 4;
static const int TN = 4;
static const int BK = 32;  // tile in K dimension

__global__ void sgemmRegisterTiled(const float* __restrict__ A,
                                   const float* __restrict__ B,
                                   float* __restrict__ C,
                                   int N) {
    // Block computes a TILE×TILE output tile.
    // threadIdx.x in [0, TILE/TN), threadIdx.y in [0, TILE/TM).
    const int tx = threadIdx.x;  // 0..7
    const int ty = threadIdx.y;  // 0..7

    // Starting row/col of this thread's micro-tile in the output.
    const int rowStart = blockIdx.y * TILE + ty * TM;
    const int colStart = blockIdx.x * TILE + tx * TN;

    // Register accumulators for the TM×TN micro-tile.
    float acc[TM][TN];
    for (int i = 0; i < TM; ++i)
        for (int j = 0; j < TN; ++j)
            acc[i][j] = 0.0f;

    // Shared tiles for A and B.
    __shared__ float As[TILE][BK];
    __shared__ float Bs[BK][TILE];

    // Total threads per block.
    const int numThreads = (TILE / TM) * (TILE / TN);  // 64
    const int tid = ty * (TILE / TN) + tx;

    for (int t = 0; t < N; t += BK) {
        // Cooperatively load As[TILE][BK] and Bs[BK][TILE].
        // Each thread loads multiple elements.
        for (int i = tid; i < TILE * BK; i += numThreads) {
            int r = i / BK;
            int c = i % BK;
            int gRow = blockIdx.y * TILE + r;
            int gCol = t + c;
            As[r][c] = (gRow < N && gCol < N) ? A[gRow * N + gCol] : 0.0f;
        }
        for (int i = tid; i < BK * TILE; i += numThreads) {
            int r = i / TILE;
            int c = i % TILE;
            int gRow = t + r;
            int gCol = blockIdx.x * TILE + c;
            Bs[r][c] = (gRow < N && gCol < N) ? B[gRow * N + gCol] : 0.0f;
        }
        __syncthreads();

        // Compute TM×TN partial products.
        for (int k = 0; k < BK; ++k) {
            float a_reg[TM], b_reg[TN];
            for (int i = 0; i < TM; ++i) a_reg[i] = As[ty * TM + i][k];
            for (int j = 0; j < TN; ++j) b_reg[j] = Bs[k][tx * TN + j];
            for (int i = 0; i < TM; ++i)
                for (int j = 0; j < TN; ++j)
                    acc[i][j] += a_reg[i] * b_reg[j];
        }
        __syncthreads();
    }

    // Write results.
    for (int i = 0; i < TM; ++i) {
        for (int j = 0; j < TN; ++j) {
            int r = rowStart + i;
            int c = colStart + j;
            if (r < N && c < N)
                C[r * N + c] = acc[i][j];
        }
    }
}

int main() {
    printf("\n===== RUNG 7: Matrix Multiply (SGEMM) =====\n\n");

    DeviceInfo info = queryDevice();

    const int N = M_DIM;
    const size_t bytes = (size_t)N * N * sizeof(float);
    const double flops = 2.0 * N * N * (double)N;  // 2N³ FLOPs (FMA = 2 ops)

    float* h_A = (float*)malloc(bytes);
    float* h_B = (float*)malloc(bytes);
    float* h_C = (float*)malloc(bytes);
    float* h_ref = (float*)malloc(bytes);

    std::srand(42);
    for (int i = 0; i < N * N; ++i) {
        h_A[i] = (float)(rand() % 100) / 100.0f;
        h_B[i] = (float)(rand() % 100) / 100.0f;
    }

    printf("  Computing CPU reference (N=%d)... ", N); fflush(stdout);
    sgemmCPU(h_A, h_B, h_ref, N);
    printf("done.\n\n");

    float *d_A, *d_B, *d_C;
    CUDA_CHECK(cudaMalloc(&d_A, bytes));
    CUDA_CHECK(cudaMalloc(&d_B, bytes));
    CUDA_CHECK(cudaMalloc(&d_C, bytes));
    CUDA_CHECK(cudaMemcpy(d_A, h_A, bytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_B, h_B, bytes, cudaMemcpyHostToDevice));

    // Increased tolerance for large matmuls (FP accumulation error).
    float absTol = N * 1e-3f;
    float relTol = 1e-2f;

    // (a) Naive.
    {
        dim3 threads(16, 16);
        dim3 grid((N + 15) / 16, (N + 15) / 16);

        CUDA_CHECK(cudaMemset(d_C, 0, bytes));
        sgemmNaive<<<grid, threads>>>(d_A, d_B, d_C, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        if (!verifyFloats(h_ref, h_C, N * N, absTol, relTol)) {
            printf("  FAIL: sgemmNaive\n"); return 1;
        }
        printf("  sgemmNaive: PASSED\n");

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            sgemmNaive<<<grid, threads>>>(d_A, d_B, d_C, N);
            t.stop();
        });
        reportGFlops("naive (1 thread/element)", res.medianMs, flops, info.peakFP32GFlops);
    }

    // (b) Tiled.
    {
        dim3 threads(TILE, TILE);
        dim3 grid((N + TILE - 1) / TILE, (N + TILE - 1) / TILE);

        CUDA_CHECK(cudaMemset(d_C, 0, bytes));
        sgemmTiled<<<grid, threads>>>(d_A, d_B, d_C, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        if (!verifyFloats(h_ref, h_C, N * N, absTol, relTol)) {
            printf("  FAIL: sgemmTiled\n"); return 1;
        }
        printf("  sgemmTiled: PASSED\n");

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            sgemmTiled<<<grid, threads>>>(d_A, d_B, d_C, N);
            t.stop();
        });
        reportGFlops("tiled (shared mem, TILE=32)", res.medianMs, flops, info.peakFP32GFlops);
    }

    // (c) Register-tiled.
    {
        dim3 threads(TILE / TN, TILE / TM);  // 8×8 = 64 threads
        dim3 grid((N + TILE - 1) / TILE, (N + TILE - 1) / TILE);

        CUDA_CHECK(cudaMemset(d_C, 0, bytes));
        sgemmRegisterTiled<<<grid, threads>>>(d_A, d_B, d_C, N);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        if (!verifyFloats(h_ref, h_C, N * N, absTol, relTol)) {
            printf("  FAIL: sgemmRegisterTiled\n"); return 1;
        }
        printf("  sgemmRegisterTiled: PASSED\n");

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            sgemmRegisterTiled<<<grid, threads>>>(d_A, d_B, d_C, N);
            t.stop();
        });
        reportGFlops("register-tiled (4x4 micro-tile)", res.medianMs, flops, info.peakFP32GFlops);
    }

    // (d) cuBLAS (optional).
#ifdef USE_CUBLAS
    {
        cublasHandle_t handle;
        cublasCreate(&handle);
        float alpha = 1.0f, beta = 0.0f;

        CUDA_CHECK(cudaMemset(d_C, 0, bytes));
        cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N,
                    N, N, N, &alpha, d_B, N, d_A, N, &beta, d_C, N);
        CUDA_CHECK(cudaDeviceSynchronize());
        CUDA_CHECK(cudaMemcpy(h_C, d_C, bytes, cudaMemcpyDeviceToHost));
        printf("  cuBLAS: %s\n",
               verifyFloats(h_ref, h_C, N * N, absTol * 2, relTol * 2) ? "PASSED" : "FAILED (tolerance)");

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            cublasSgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N,
                        N, N, N, &alpha, d_B, N, d_A, N, &beta, d_C, N);
            t.stop();
        });
        reportGFlops("cuBLAS sgemm", res.medianMs, flops, info.peakFP32GFlops);

        cublasDestroy(handle);
    }
#else
    printf("\n  cuBLAS comparison skipped (build with -DUSE_CUBLAS and link -lcublas)\n");
#endif

    printf("\n  This kernel is COMPUTE-BOUND (arithmetic intensity = N/6 FLOP/byte).\n");
    printf("  Tiling and register blocking increase data reuse.\n\n");

    CUDA_CHECK(cudaFree(d_A));
    CUDA_CHECK(cudaFree(d_B));
    CUDA_CHECK(cudaFree(d_C));
    free(h_A); free(h_B); free(h_C); free(h_ref);

    return 0;
}
