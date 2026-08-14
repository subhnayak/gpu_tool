// 10_occupancy.cu — Rung 10: Occupancy vs ILP Tradeoff
// ============================================================================
// LESSON: Maximum occupancy is NOT always fastest (the Volkov argument).
//
// A kernel that uses more registers per thread has lower occupancy but can
// do more independent work per thread (ILP), keeping the ALUs busy via
// instruction-level parallelism rather than thread-level parallelism.
//
// We demonstrate this by varying the unroll factor of a compute-heavy kernel
// using __launch_bounds__ and template parameters:
//   - High occupancy (few registers): many warps, each doing little work.
//   - Low occupancy (many registers): fewer warps, each doing more ILP.
//
// INTERVIEW: "Is maximum occupancy always optimal?"
//            "Explain the occupancy vs ILP tradeoff."
//            "What did Vasily Volkov show?"
//
// We also use cudaOccupancyMaxActiveBlocksPerMultiprocessor to print
// theoretical occupancy.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "common/helpers.cuh"

// ============================================================================
// Compute-heavy kernel: independent FMA chains to exploit ILP.
// Template parameter DEPTH controls how many independent accumulations each
// thread maintains.  More depth → more registers → lower occupancy but more
// ILP.
// ============================================================================

template <int DEPTH>
__global__ void computeILP(const float* __restrict__ in,
                           float* __restrict__ out,
                           int N, int iters) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N) return;

    float val = in[idx];

    // DEPTH independent accumulators — each is a separate FMA chain.
    float acc[DEPTH];
    for (int d = 0; d < DEPTH; ++d)
        acc[d] = val + (float)d * 0.01f;

    // Iterate: each accumulator does independent work.
    for (int iter = 0; iter < iters; ++iter) {
        for (int d = 0; d < DEPTH; ++d)
            acc[d] = acc[d] * 1.00001f + 0.00001f;  // FMA
    }

    // Reduce accumulators to a single value (prevent compiler from optimizing away).
    float result = 0.0f;
    for (int d = 0; d < DEPTH; ++d)
        result += acc[d];

    out[idx] = result;
}

// ============================================================================
// Constrained kernel: __launch_bounds__ forces a specific register limit.
// maxThreadsPerBlock = 256, minBlocksPerMultiprocessor = varies.
// ============================================================================
template <int DEPTH>
__global__ void __launch_bounds__(256, 4)
computeHighOcc(const float* __restrict__ in,
               float* __restrict__ out,
               int N, int iters) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= N) return;

    float val = in[idx];
    float acc[DEPTH];
    for (int d = 0; d < DEPTH; ++d)
        acc[d] = val + (float)d * 0.01f;

    for (int iter = 0; iter < iters; ++iter) {
        for (int d = 0; d < DEPTH; ++d)
            acc[d] = acc[d] * 1.00001f + 0.00001f;
    }

    float result = 0.0f;
    for (int d = 0; d < DEPTH; ++d)
        result += acc[d];
    out[idx] = result;
}

// ============================================================================
template <int DEPTH, typename KernelFunc>
void runVariant(const char* name, KernelFunc kernel,
                float* d_in, float* d_out, int N, int iters,
                const DeviceInfo& info) {
    const int TPB = 256;
    const int blocks = (N + TPB - 1) / TPB;

    // Query theoretical occupancy.
    int maxActiveBlocks = 0;
    CUDA_CHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &maxActiveBlocks, kernel, TPB, 0));

    int maxWarpsPerSM = 0;
    {
        cudaDeviceProp prop;
        CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
        maxWarpsPerSM = prop.maxThreadsPerMultiProcessor / 32;
    }
    int activeWarps = maxActiveBlocks * (TPB / 32);
    float occupancy = (float)activeWarps / maxWarpsPerSM;

    // Run and verify.
    kernel<<<blocks, TPB>>>(d_in, d_out, N, iters);
    CUDA_CHECK_KERNEL();

    // Benchmark.
    BenchResult res = benchmark([&](GpuTimer& t) {
        t.start();
        kernel<<<blocks, TPB>>>(d_in, d_out, N, iters);
        t.stop();
    });

    // FLOPs: each element does DEPTH * iters * 2 (FMA) + DEPTH adds.
    double flops = (double)N * DEPTH * iters * 2.0;
    double gflops = flops / (res.medianMs / 1000.0) / 1e9;
    double pctPeak = 100.0 * gflops / info.peakFP32GFlops;

    printf("  %-40s  occ=%5.1f%%  %8.3f ms  %8.1f GFLOP/s  (%5.1f%% peak)\n",
           name, occupancy * 100.0f, res.medianMs, gflops, pctPeak);
}

int main() {
    printf("\n===== RUNG 10: Occupancy vs ILP =====\n\n");

    DeviceInfo info = queryDevice();

    const int N = 1 << 20;  // 1M elements
    const int ITERS = 200;  // enough iterations to be compute-bound
    const size_t bytes = N * sizeof(float);

    float* h_in = (float*)malloc(bytes);
    for (int i = 0; i < N; ++i) h_in[i] = 1.0f;

    float *d_in, *d_out;
    CUDA_CHECK(cudaMalloc(&d_in, bytes));
    CUDA_CHECK(cudaMalloc(&d_out, bytes));
    CUDA_CHECK(cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice));

    printf("  Each variant uses a different ILP depth (# independent FMA chains).\n");
    printf("  More depth → more registers → lower occupancy → but more ILP.\n\n");

    printf("  %-40s  %7s  %8s  %12s  %10s\n",
           "Variant", "Occ", "Time", "GFLOP/s", "% Peak");
    printf("  %-40s  %7s  %8s  %12s  %10s\n",
           "-------", "---", "----", "-------", "------");

    // Unconstrained variants with increasing ILP depth.
    runVariant<1>("ILP depth=1 (unconstrained)", computeILP<1>,
                  d_in, d_out, N, ITERS, info);
    runVariant<2>("ILP depth=2 (unconstrained)", computeILP<2>,
                  d_in, d_out, N, ITERS, info);
    runVariant<4>("ILP depth=4 (unconstrained)", computeILP<4>,
                  d_in, d_out, N, ITERS, info);
    runVariant<8>("ILP depth=8 (unconstrained)", computeILP<8>,
                  d_in, d_out, N, ITERS, info);
    runVariant<16>("ILP depth=16 (unconstrained)", computeILP<16>,
                   d_in, d_out, N, ITERS, info);

    printf("\n  --- With __launch_bounds__(256, 4) forcing high occupancy ---\n\n");

    runVariant<1>("ILP depth=1 (launch_bounds)", computeHighOcc<1>,
                  d_in, d_out, N, ITERS, info);
    runVariant<4>("ILP depth=4 (launch_bounds)", computeHighOcc<4>,
                  d_in, d_out, N, ITERS, info);
    runVariant<8>("ILP depth=8 (launch_bounds)", computeHighOcc<8>,
                  d_in, d_out, N, ITERS, info);

    printf("\n");
    printf("  THE VOLKOV ARGUMENT: Lower occupancy with more ILP per thread\n");
    printf("  can outperform higher occupancy with less ILP, because the ALUs\n");
    printf("  stay busy via instruction-level parallelism rather than relying\n");
    printf("  solely on thread-level parallelism for latency hiding.\n");
    printf("  Look for the sweet spot — usually NOT 100%% occupancy.\n\n");
    printf("  Profile: ncu --set full -o occupancy ./10_occupancy\n");
    printf("  Metrics: sm__warps_active.avg.pct_of_peak_sustained_active\n");
    printf("           smsp__inst_executed_pipe_fma.avg.pct_of_peak_sustained_active\n\n");

    CUDA_CHECK(cudaFree(d_in));
    CUDA_CHECK(cudaFree(d_out));
    free(h_in);

    return 0;
}
