// 03_transpose.cu — Rung 3: Matrix Transpose
// ============================================================================
// LESSON: Shared memory banks and tiling for optimal access patterns.
//
// A naive transpose reads rows (coalesced) but writes columns (strided by
// the matrix width → uncoalesced).  By tiling through shared memory we can
// read a tile in coalesced order, store it in shared memory, synchronize,
// then write it in coalesced order from shared memory.
//
// The twist: shared memory has 32 banks.  A 32×32 tile has column accesses
// that all hit the same bank → 32-way bank conflict.  Adding 1 column of
// padding (float tile[32][33]) eliminates the conflict entirely.
//
// INTERVIEW QUESTIONS:
//   - "Explain shared memory bank conflicts and how to avoid them."
//   - "How would you optimize a matrix transpose on a GPU?"
//   - ncu metric: l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_ld.sum
//
// EXPECTED:
//   Naive transpose: ~50% of copy bandwidth.
//   Tiled (no pad):  ~70-85% (bank conflicts limit it).
//   Tiled (padded):  ~85-95% — nearly matching a simple copy.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "common/helpers.cuh"

static const int TILE_DIM  = 32;
static const int BLOCK_ROWS = 8;  // Each block has TILE_DIM × BLOCK_ROWS threads.

// ---- Simple copy kernel (upper bound on bandwidth) ----
__global__ void copyKernel(const float* __restrict__ in,
                           float* __restrict__ out,
                           int width, int height) {
    int x = blockIdx.x * TILE_DIM + threadIdx.x;
    int y = blockIdx.y * TILE_DIM + threadIdx.y;
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (x < width && (y + j) < height) {
            out[(y + j) * width + x] = in[(y + j) * width + x];
        }
    }
}

// ---- Naive transpose: coalesced reads, strided writes ----
// Thread (tx, ty) reads from in[row][col] and writes to out[col][row].
// The reads are coalesced (consecutive tx → consecutive cols in the same row).
// The writes are strided (consecutive tx → addresses stride 'height' apart).
__global__ void transposeNaive(const float* __restrict__ in,
                               float* __restrict__ out,
                               int width, int height) {
    int x = blockIdx.x * TILE_DIM + threadIdx.x;
    int y = blockIdx.y * TILE_DIM + threadIdx.y;
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (x < width && (y + j) < height) {
            out[x * height + (y + j)] = in[(y + j) * width + x];
        }
    }
}

// ---- Tiled transpose (shared memory, NO padding) ----
// Read tile coalesced into shared memory, __syncthreads(), then write
// transposed tile coalesced from shared memory.
// This version has bank conflicts when reading the shared memory column.
__global__ void transposeTiled(const float* __restrict__ in,
                               float* __restrict__ out,
                               int width, int height) {
    __shared__ float tile[TILE_DIM][TILE_DIM];  // 32 banks, column access = conflict!

    int x = blockIdx.x * TILE_DIM + threadIdx.x;
    int y = blockIdx.y * TILE_DIM + threadIdx.y;

    // Load tile from global to shared (coalesced read).
    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (x < width && (y + j) < height) {
            tile[threadIdx.y + j][threadIdx.x] = in[(y + j) * width + x];
        }
    }

    __syncthreads();  // Must complete before any thread reads from tile.

    // Write transposed tile (coalesced write).
    // Note: we swap blockIdx.x and blockIdx.y for the output position.
    int outX = blockIdx.y * TILE_DIM + threadIdx.x;
    int outY = blockIdx.x * TILE_DIM + threadIdx.y;

    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (outX < height && (outY + j) < width) {
            // Reading tile[threadIdx.x][threadIdx.y+j]: threadIdx.x varies
            // across threads in a warp → column access → BANK CONFLICT.
            out[(outY + j) * height + outX] = tile[threadIdx.x][threadIdx.y + j];
        }
    }
}

// ---- Tiled transpose with PADDING to avoid bank conflicts ----
// By making the shared memory array tile[32][33], consecutive columns
// map to different banks.  The extra column wastes 32 floats (128 bytes)
// per tile but eliminates ALL bank conflicts.
__global__ void transposeTiledPadded(const float* __restrict__ in,
                                     float* __restrict__ out,
                                     int width, int height) {
    __shared__ float tile[TILE_DIM][TILE_DIM + 1];  // +1 padding!

    int x = blockIdx.x * TILE_DIM + threadIdx.x;
    int y = blockIdx.y * TILE_DIM + threadIdx.y;

    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (x < width && (y + j) < height) {
            tile[threadIdx.y + j][threadIdx.x] = in[(y + j) * width + x];
        }
    }

    __syncthreads();

    int outX = blockIdx.y * TILE_DIM + threadIdx.x;
    int outY = blockIdx.x * TILE_DIM + threadIdx.y;

    for (int j = 0; j < TILE_DIM; j += BLOCK_ROWS) {
        if (outX < height && (outY + j) < width) {
            // Same column access pattern, but padding shifts each column
            // to a different bank → NO conflicts.
            out[(outY + j) * height + outX] = tile[threadIdx.x][threadIdx.y + j];
        }
    }
}

// CPU reference.
void transposeCPU(const float* in, float* out, int width, int height) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            out[x * height + y] = in[y * width + x];
}

int main() {
    printf("\n===== RUNG 3: Matrix Transpose =====\n\n");

    DeviceInfo info = queryDevice();

    const int WIDTH  = 4096;
    const int HEIGHT = 4096;
    const size_t bytes = WIDTH * HEIGHT * sizeof(float);
    const double totalBytes = 2.0 * bytes;  // 1 load + 1 store

    float* h_in  = (float*)malloc(bytes);
    float* h_out = (float*)malloc(bytes);
    float* h_ref = (float*)malloc(bytes);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) h_in[i] = (float)(i % 1000);

    transposeCPU(h_in, h_ref, WIDTH, HEIGHT);

    float *d_in, *d_out;
    CUDA_CHECK(cudaMalloc(&d_in, bytes));
    CUDA_CHECK(cudaMalloc(&d_out, bytes));
    CUDA_CHECK(cudaMemcpy(d_in, h_in, bytes, cudaMemcpyHostToDevice));

    dim3 threads(TILE_DIM, BLOCK_ROWS);
    dim3 grid((WIDTH + TILE_DIM - 1) / TILE_DIM,
              (HEIGHT + TILE_DIM - 1) / TILE_DIM);

    // Helper lambda to verify and bench a kernel.
    auto runKernel = [&](const char* name, auto kernel, bool isTranspose) {
        CUDA_CHECK(cudaMemset(d_out, 0, bytes));
        kernel<<<grid, threads>>>(d_in, d_out, WIDTH, HEIGHT);
        CUDA_CHECK_KERNEL();
        CUDA_CHECK(cudaMemcpy(h_out, d_out, bytes, cudaMemcpyDeviceToHost));

        const float* ref = isTranspose ? h_ref : h_in;
        if (!verifyFloats(ref, h_out, WIDTH * HEIGHT)) {
            printf("  FAIL: %s\n", name);
            return;
        }
        printf("  %s: PASSED\n", name);

        BenchResult res = benchmark([&](GpuTimer& t) {
            t.start();
            kernel<<<grid, threads>>>(d_in, d_out, WIDTH, HEIGHT);
            t.stop();
        });
        reportBandwidth(name, res.medianMs, totalBytes, info.peakBandwidthGBs);
    };

    runKernel("copy (upper bound)",         copyKernel,           false);
    runKernel("naive transpose",            transposeNaive,       true);
    runKernel("tiled (bank conflicts)",     transposeTiled,       true);
    runKernel("tiled + padded (no conflicts)", transposeTiledPadded, true);

    printf("\n  Profile bank conflicts with:\n");
    printf("    ncu --metrics l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_ld.sum ./03_transpose\n");
    printf("  The padded version should show 0 conflicts.\n\n");

    CUDA_CHECK(cudaFree(d_in));
    CUDA_CHECK(cudaFree(d_out));
    free(h_in); free(h_out); free(h_ref);

    return 0;
}
