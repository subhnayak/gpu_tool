/*
 * sample_kernel.cu — A small CUDA kernel with interesting constructs.
 *
 * Contains: a loop, a conditional, shared memory, an atomic operation,
 * and a __syncthreads(), so the generated PTX and SASS will have
 * interesting patterns to study.
 *
 * Compile:
 *   nvcc -ptx -arch=sm_89 -lineinfo sample_kernel.cu -o sample_kernel.ptx
 *   nvcc -cubin -arch=sm_89 -lineinfo sample_kernel.cu -o sample_kernel.cubin
 */

#include <cstdint>

// Shared memory reduction kernel with interesting control flow
__global__ void reduce_sum(const float* __restrict__ input,
                           float* __restrict__ output,
                           int n)
{
    // Shared memory for block-level reduction
    __shared__ float smem[256];

    int tid = threadIdx.x;
    int gid = blockIdx.x * blockDim.x + threadIdx.x;

    // Conditional load with bounds check (generates predicated branch)
    float val = 0.0f;
    if (gid < n) {
        val = input[gid];
    }
    smem[tid] = val;

    __syncthreads();  // Generates BAR instruction in SASS

    // Reduction loop (generates a loop in PTX/SASS with back-edge)
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            smem[tid] += smem[tid + stride];
        }
        __syncthreads();
    }

    // Thread 0 writes result with atomic add (generates ATOMS/RED in SASS)
    if (tid == 0) {
        atomicAdd(output, smem[0]);
    }
}

// A second kernel with different patterns
__global__ void saxpy(float a, const float* x, const float* y, float* out, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        out[i] = a * x[i] + y[i];
    }
}
