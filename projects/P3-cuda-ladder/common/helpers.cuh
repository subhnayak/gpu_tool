// helpers.cuh — Shared infrastructure for the CUDA Ladder project.
// Provides error checking, timing, verification, device-info printing,
// and a benchmark runner.  Include this in every rung.
#pragma once

#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <numeric>
#include <string>

// ============================================================================
// 1.  CUDA ERROR-CHECKING MACROS
// ============================================================================
// CUDA_CHECK(x):  Wraps any CUDA API call.  Prints file, line, and the
//                 human-readable error string, then aborts.
// CUDA_CHECK_KERNEL(): Call IMMEDIATELY after a kernel launch.  It calls
//                 cudaGetLastError() (catches launch config errors) and then
//                 cudaDeviceSynchronize() (catches asynchronous execution errors).
//
// Interview tie-in: "How do you debug CUDA launch failures?"
// Answer: always wrap API calls and post-launch with error checks.

#define CUDA_CHECK(call)                                                       \
    do {                                                                       \
        cudaError_t err_ = (call);                                             \
        if (err_ != cudaSuccess) {                                             \
            fprintf(stderr, "CUDA error at %s:%d — %s\n  Expression: %s\n",   \
                    __FILE__, __LINE__, cudaGetErrorString(err_), #call);       \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

#define CUDA_CHECK_KERNEL()                                                    \
    do {                                                                       \
        cudaError_t err_ = cudaGetLastError();                                 \
        if (err_ != cudaSuccess) {                                             \
            fprintf(stderr, "Kernel launch error at %s:%d — %s\n",            \
                    __FILE__, __LINE__, cudaGetErrorString(err_));              \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
        err_ = cudaDeviceSynchronize();                                        \
        if (err_ != cudaSuccess) {                                             \
            fprintf(stderr, "Kernel execution error at %s:%d — %s\n",         \
                    __FILE__, __LINE__, cudaGetErrorString(err_));              \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

// ============================================================================
// 2.  GpuTimer — RAII-style timer using CUDA events
// ============================================================================
// CUDA events are recorded on the GPU timeline, so they measure only device
// work — not host overhead.  This is the correct way to time kernels.
//
// Usage:
//   GpuTimer timer;
//   timer.start();
//   myKernel<<<grid, block>>>(...);
//   timer.stop();
//   float ms = timer.elapsed();

struct GpuTimer {
    cudaEvent_t startEv, stopEv;

    GpuTimer() {
        CUDA_CHECK(cudaEventCreate(&startEv));
        CUDA_CHECK(cudaEventCreate(&stopEv));
    }
    ~GpuTimer() {
        cudaEventDestroy(startEv);
        cudaEventDestroy(stopEv);
    }
    // Record start on the current stream (0 = default).
    void start(cudaStream_t stream = 0) {
        CUDA_CHECK(cudaEventRecord(startEv, stream));
    }
    // Record stop and synchronise so elapsed() is valid.
    void stop(cudaStream_t stream = 0) {
        CUDA_CHECK(cudaEventRecord(stopEv, stream));
        CUDA_CHECK(cudaEventSynchronize(stopEv));
    }
    // Returns wall-clock milliseconds between start and stop.
    float elapsed() const {
        float ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&ms, startEv, stopEv));
        return ms;
    }

    // Non-copyable.
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
};

// ============================================================================
// 3.  Benchmark runner — warm-up + N iterations, return median time
// ============================================================================
// The callable `fn` is invoked with no arguments; it should contain the kernel
// launch (and any synchronization the timer needs).  The function itself must
// NOT include synchronization after the launch—the GpuTimer::stop() handles it.
//
// Returns a struct with min, median, and mean times.

struct BenchResult {
    float minMs;
    float medianMs;
    float meanMs;
};

// `fn` signature: void fn(GpuTimer& timer)
//   — the callable MUST call timer.start() before and timer.stop() after the
//     kernel launch so that timing is accurate.
template <typename Fn>
BenchResult benchmark(Fn fn, int warmup = 3, int iters = 10) {
    GpuTimer timer;

    // Warm-up: bring caches and driver state to steady-state.
    for (int i = 0; i < warmup; ++i) {
        fn(timer);
    }

    std::vector<float> times(iters);
    for (int i = 0; i < iters; ++i) {
        fn(timer);
        times[i] = timer.elapsed();
    }

    std::sort(times.begin(), times.end());
    float minMs  = times.front();
    float medMs  = times[iters / 2];
    float meanMs = 0.0f;
    for (float t : times) meanMs += t;
    meanMs /= iters;

    return {minMs, medMs, meanMs};
}

// ============================================================================
// 4.  Device-property query — print everything relevant to performance
// ============================================================================
// Computes theoretical peaks using the standard formulas:
//   Bandwidth (GB/s) = memClockMHz * 1e6 * (busWidth/8) * 2 (DDR) / 1e9
//   FP32 peak GFLOP/s = SMs * coresPerSM * 2 * clockGHz
//
// coresPerSM depends on compute capability.

struct DeviceInfo {
    int smCount;
    int coresPerSM;
    float clockGHz;
    float memClockGHz;
    int busWidthBits;
    float peakBandwidthGBs;   // theoretical
    float peakFP32GFlops;     // theoretical
    std::string name;
};

inline int getCoresPerSM(int major, int minor) {
    // Reference: CUDA programming guide, Table "Technical Specifications".
    switch (major) {
        case 2: return (minor == 1) ? 48 : 32;  // Fermi
        case 3: return 192;                       // Kepler
        case 5: return 128;                       // Maxwell
        case 6: return (minor == 0) ? 64 : 128;  // Pascal
        case 7: return 64;                        // Volta / Turing
        case 8: return (minor == 0) ? 64 : 128;  // Ampere (GA100 vs GA10x)
        case 9: return 128;                       // Hopper
        case 10: return 128;                      // Blackwell (tentative)
        default: return 128;
    }
}

inline DeviceInfo queryDevice(int dev = 0) {
    cudaDeviceProp prop;
    CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));

    DeviceInfo info;
    info.name         = prop.name;
    info.smCount      = prop.multiProcessorCount;
    info.coresPerSM   = getCoresPerSM(prop.major, prop.minor);
    info.clockGHz     = prop.clockRate / 1.0e6f;            // clockRate is in kHz
    info.memClockGHz  = prop.memoryClockRate / 1.0e6f;      // kHz → GHz
    info.busWidthBits = prop.memoryBusWidth;

    // Theoretical peak memory bandwidth:
    //   memClockHz * (busWidth bytes) * 2 (DDR) / 1e9
    info.peakBandwidthGBs = prop.memoryClockRate * 1e3f     // kHz → Hz
                          * (prop.memoryBusWidth / 8.0f)    // bits → bytes
                          * 2.0f                            // DDR
                          / 1.0e9f;                         // → GB/s

    // Theoretical FP32 peak:
    //   SMs * coresPerSM * 2 (FMA) * clockHz / 1e9
    info.peakFP32GFlops = (float)info.smCount * info.coresPerSM
                        * 2.0f                              // FMA = 2 ops
                        * (prop.clockRate * 1e3f)           // kHz → Hz
                        / 1.0e9f;                           // → GFLOP/s

    printf("=== Device %d: %s ===\n", dev, prop.name);
    printf("  Compute capability : %d.%d\n", prop.major, prop.minor);
    printf("  SMs                : %d\n", info.smCount);
    printf("  Cores/SM           : %d  (total %d)\n",
           info.coresPerSM, info.smCount * info.coresPerSM);
    printf("  GPU clock          : %.2f GHz\n", info.clockGHz);
    printf("  Memory clock       : %.2f GHz\n", info.memClockGHz);
    printf("  Memory bus width   : %d bits\n", info.busWidthBits);
    printf("  Peak bandwidth     : %.1f GB/s\n", info.peakBandwidthGBs);
    printf("  Peak FP32 throughput: %.1f GFLOP/s\n", info.peakFP32GFlops);
    printf("  L2 cache size      : %d KB\n", prop.l2CacheSize / 1024);
    printf("  Shared mem/block   : %u bytes\n", (unsigned)prop.sharedMemPerBlock);
    printf("  Max threads/block  : %d\n", prop.maxThreadsPerBlock);
    printf("  Warp size          : %d\n", prop.warpSize);
    printf("  Registers/block    : %d\n", prop.regsPerBlock);
    printf("\n");

    return info;
}

// ============================================================================
// 5.  Verification helpers
// ============================================================================
// Compare two float arrays with a relative + absolute tolerance.
// Returns true if all elements match; on failure prints the first mismatch.

inline bool verifyFloats(const float* ref, const float* test, size_t n,
                         float absTol = 1e-4f, float relTol = 1e-4f) {
    for (size_t i = 0; i < n; ++i) {
        float diff = fabsf(ref[i] - test[i]);
        float denom = fmaxf(fabsf(ref[i]), fabsf(test[i]));
        if (diff > absTol && diff > relTol * denom) {
            printf("  MISMATCH at index %zu: ref=%.6f  test=%.6f  diff=%.6e\n",
                   i, ref[i], test[i], diff);
            return false;
        }
    }
    return true;
}

inline bool verifyInts(const int* ref, const int* test, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (ref[i] != test[i]) {
            printf("  MISMATCH at index %zu: ref=%d  test=%d\n",
                   i, ref[i], test[i]);
            return false;
        }
    }
    return true;
}

// ============================================================================
// 6.  Results-reporting function
// ============================================================================
// Prints a formatted one-liner:
//   [name]  time  GB/s  % of peak   (or GFLOP/s for compute-bound)
//
// `bytes` > 0 → report bandwidth; `flops` > 0 → report GFLOP/s.
// If both are >0, bandwidth takes precedence in the "% peak" column.

inline void reportBandwidth(const char* name, float ms,
                            double bytes, float peakGBs) {
    double seconds = ms / 1000.0;
    double gbPerSec = bytes / seconds / 1.0e9;
    double pctPeak = 100.0 * gbPerSec / peakGBs;
    printf("  %-40s %8.3f ms  %8.1f GB/s  (%5.1f%% peak)\n",
           name, ms, gbPerSec, pctPeak);
}

inline void reportGFlops(const char* name, float ms,
                         double flops, float peakGFlops) {
    double seconds = ms / 1000.0;
    double gflops = flops / seconds / 1.0e9;
    double pctPeak = 100.0 * gflops / peakGFlops;
    printf("  %-40s %8.3f ms  %8.1f GFLOP/s  (%5.1f%% peak)\n",
           name, ms, gflops, pctPeak);
}

// Combined: pass which metric to use.
enum MetricType { METRIC_BW, METRIC_FLOPS };

inline void reportResult(const char* name, float ms,
                         double amount, float peak, MetricType mt) {
    if (mt == METRIC_BW) {
        reportBandwidth(name, ms, amount, peak);
    } else {
        reportGFlops(name, ms, amount, peak);
    }
}
