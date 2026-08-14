// 09_streams.cu — Rung 9: Streams, Pinned Memory, and CUDA Graphs
// ============================================================================
// LESSON: The PCIe bus is a bottleneck.  Overlapping transfers with compute
// hides latency.  Pinned (page-locked) memory enables async DMA.
//
// We demonstrate:
//   (a) Pageable vs pinned memory transfer bandwidth.
//   (b) Sequential: H2D → kernel → D2H (no overlap).
//   (c) Pipelined: chunk the work across multiple streams so H2D, compute,
//       and D2H overlap.
//   (d) CUDA Graphs: capturing a multi-kernel workload as a graph to reduce
//       launch overhead.
//
// INTERVIEW: "How do you overlap transfers and compute on a GPU?"
//            "What is pinned memory and why does it matter?"
//            "What are CUDA graphs?"
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include "common/helpers.cuh"

static const int TPB = 256;

// Simple compute kernel (heavy enough to show overlap).
__global__ void computeKernel(const float* __restrict__ in,
                              float* __restrict__ out, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        float val = in[i];
        // Do enough work to not be trivially fast.
        for (int iter = 0; iter < 50; ++iter)
            val = sinf(val) * cosf(val) + 0.001f;
        out[i] = val;
    }
}

// CPU reference.
void computeCPU(const float* in, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        float val = in[i];
        for (int iter = 0; iter < 50; ++iter)
            val = sinf(val) * cosf(val) + 0.001f;
        out[i] = val;
    }
}

int main() {
    printf("\n===== RUNG 9: Streams & Pinned Memory =====\n\n");

    DeviceInfo info = queryDevice();

    const int N = 1 << 22;  // 4M elements
    const size_t bytes = N * sizeof(float);

    // ========================================================================
    // PART 1: Pageable vs Pinned transfer bandwidth.
    // ========================================================================
    printf("  --- Transfer Bandwidth ---\n");
    {
        float* h_pageable = (float*)malloc(bytes);
        float* h_pinned;
        CUDA_CHECK(cudaMallocHost(&h_pinned, bytes));
        float* d_buf;
        CUDA_CHECK(cudaMalloc(&d_buf, bytes));
        for (int i = 0; i < N; ++i) { h_pageable[i] = 1.0f; h_pinned[i] = 1.0f; }

        // H2D pageable.
        BenchResult r1 = benchmark([&](GpuTimer& t) {
            t.start();
            CUDA_CHECK(cudaMemcpy(d_buf, h_pageable, bytes, cudaMemcpyHostToDevice));
            t.stop();
        });
        reportBandwidth("H2D pageable", r1.medianMs, bytes, info.peakBandwidthGBs);

        // H2D pinned.
        BenchResult r2 = benchmark([&](GpuTimer& t) {
            t.start();
            CUDA_CHECK(cudaMemcpy(d_buf, h_pinned, bytes, cudaMemcpyHostToDevice));
            t.stop();
        });
        reportBandwidth("H2D pinned", r2.medianMs, bytes, info.peakBandwidthGBs);

        printf("  Pinned speedup: %.2fx\n\n", r1.medianMs / r2.medianMs);

        free(h_pageable);
        CUDA_CHECK(cudaFreeHost(h_pinned));
        CUDA_CHECK(cudaFree(d_buf));
    }

    // ========================================================================
    // PART 2: Sequential vs Pipelined execution.
    // ========================================================================
    printf("  --- Sequential vs Pipelined ---\n");

    float* h_in;
    float* h_out;
    CUDA_CHECK(cudaMallocHost(&h_in, bytes));
    CUDA_CHECK(cudaMallocHost(&h_out, bytes));
    for (int i = 0; i < N; ++i) h_in[i] = 0.5f;

    float *d_in, *d_out;
    CUDA_CHECK(cudaMalloc(&d_in, bytes));
    CUDA_CHECK(cudaMalloc(&d_out, bytes));

    int blocks = (N + TPB - 1) / TPB;

    // Verify correctness first (sequential).
    CUDA_CHECK(cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice));
    computeKernel<<<blocks, TPB>>>(d_in, d_out, N);
    CUDA_CHECK_KERNEL();
    CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));

    // Quick CPU check on first few elements.
    float ref_val = 0.5f;
    for (int iter = 0; iter < 50; ++iter) ref_val = sinf(ref_val) * cosf(ref_val) + 0.001f;
    if (fabsf(h_out[0] - ref_val) > 1e-3f) {
        printf("  FAIL: compute kernel mismatch: got %f expected %f\n", h_out[0], ref_val);
        return 1;
    }
    printf("  Compute kernel: PASSED verification\n\n");

    // Sequential timing.
    BenchResult seqRes = benchmark([&](GpuTimer& t) {
        t.start();
        CUDA_CHECK(cudaMemcpyAsync(d_in, h_in, bytes, cudaMemcpyHostToDevice, 0));
        computeKernel<<<blocks, TPB, 0, 0>>>(d_in, d_out, N);
        CUDA_CHECK(cudaMemcpyAsync(h_out, d_out, bytes, cudaMemcpyDeviceToHost, 0));
        t.stop();
    });
    printf("  %-40s %8.3f ms\n", "sequential (H2D→compute→D2H)", seqRes.medianMs);

    // Pipelined with NUM_STREAMS streams.
    const int NUM_STREAMS = 4;
    cudaStream_t streams[NUM_STREAMS];
    for (int s = 0; s < NUM_STREAMS; ++s)
        CUDA_CHECK(cudaStreamCreate(&streams[s]));

    int chunkN = (N + NUM_STREAMS - 1) / NUM_STREAMS;
    size_t chunkBytes = chunkN * sizeof(float);

    BenchResult pipRes = benchmark([&](GpuTimer& t) {
        t.start();
        for (int s = 0; s < NUM_STREAMS; ++s) {
            int offset = s * chunkN;
            int thisN = (offset + chunkN <= N) ? chunkN : (N - offset);
            if (thisN <= 0) continue;
            size_t thisBytes = thisN * sizeof(float);
            int thisBlocks = (thisN + TPB - 1) / TPB;

            CUDA_CHECK(cudaMemcpyAsync(d_in + offset, h_in + offset,
                                       thisBytes, cudaMemcpyHostToDevice, streams[s]));
            computeKernel<<<thisBlocks, TPB, 0, streams[s]>>>(
                d_in + offset, d_out + offset, thisN);
            CUDA_CHECK(cudaMemcpyAsync(h_out + offset, d_out + offset,
                                       thisBytes, cudaMemcpyDeviceToHost, streams[s]));
        }
        t.stop();
    });
    printf("  %-40s %8.3f ms\n", "pipelined (4 streams)", pipRes.medianMs);
    printf("  Pipeline speedup: %.2fx\n\n", seqRes.medianMs / pipRes.medianMs);

    // ========================================================================
    // PART 3: CUDA Graphs — reduce launch overhead.
    // ========================================================================
    printf("  --- CUDA Graphs ---\n");
    {
        // Capture a sequence of small kernel launches into a graph.
        const int NUM_LAUNCHES = 20;
        const int smallN = 1024;
        const int smallBlocks = (smallN + TPB - 1) / TPB;

        float *d_small_in, *d_small_out;
        CUDA_CHECK(cudaMalloc(&d_small_in, smallN * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&d_small_out, smallN * sizeof(float)));
        CUDA_CHECK(cudaMemset(d_small_in, 0, smallN * sizeof(float)));

        // Without graph.
        BenchResult noGraph = benchmark([&](GpuTimer& t) {
            t.start();
            for (int k = 0; k < NUM_LAUNCHES; ++k)
                computeKernel<<<smallBlocks, TPB>>>(d_small_in, d_small_out, smallN);
            t.stop();
        });
        printf("  %-40s %8.3f ms\n", "20 small kernels (no graph)", noGraph.medianMs);

        // With graph.
        cudaGraph_t graph;
        cudaGraphExec_t graphExec;

        CUDA_CHECK(cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeGlobal));
        for (int k = 0; k < NUM_LAUNCHES; ++k)
            computeKernel<<<smallBlocks, TPB, 0, streams[0]>>>(d_small_in, d_small_out, smallN);
        CUDA_CHECK(cudaStreamEndCapture(streams[0], &graph));
#if CUDART_VERSION >= 12000
        CUDA_CHECK(cudaGraphInstantiate(&graphExec, graph, 0));
#else
        CUDA_CHECK(cudaGraphInstantiate(&graphExec, graph, NULL, NULL, 0));
#endif

        BenchResult withGraph = benchmark([&](GpuTimer& t) {
            t.start();
            CUDA_CHECK(cudaGraphLaunch(graphExec, streams[0]));
            t.stop();
        });
        printf("  %-40s %8.3f ms\n", "20 small kernels (CUDA graph)", withGraph.medianMs);
        printf("  Graph speedup: %.2fx\n\n", noGraph.medianMs / withGraph.medianMs);

        CUDA_CHECK(cudaGraphExecDestroy(graphExec));
        CUDA_CHECK(cudaGraphDestroy(graph));
        CUDA_CHECK(cudaFree(d_small_in));
        CUDA_CHECK(cudaFree(d_small_out));
    }

    // Cleanup.
    for (int s = 0; s < NUM_STREAMS; ++s)
        CUDA_CHECK(cudaStreamDestroy(streams[s]));
    CUDA_CHECK(cudaFreeHost(h_in));
    CUDA_CHECK(cudaFreeHost(h_out));
    CUDA_CHECK(cudaFree(d_in));
    CUDA_CHECK(cudaFree(d_out));

    printf("  Profile with: nsys profile -o streams_report ./09_streams\n");
    printf("  Look for H2D/D2H and kernel bars overlapping on the timeline.\n\n");

    return 0;
}
