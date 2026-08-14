"""
kernel.py — GPU Kernel Description
====================================

Describes the characteristics of a GPU kernel: grid/block dimensions,
resource usage, and work profile (bytes, FLOPs, instruction mix).

This is what you'd extract from a profiler (Nsight Compute) or
compute by analyzing the kernel source code.

INTERVIEW: "How do you characterize a GPU kernel for performance analysis?"
You need: grid/block dims, registers per thread, shared memory per block,
bytes read/written, FLOPs, and the dependency chain structure.
"""

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class KernelDescriptor:
    """
    Description of a GPU kernel's resource usage and work profile.

    INTERVIEW: "What information do you need to predict kernel performance?"
    1. Resource usage (for occupancy): regs, shared mem, block size
    2. Memory traffic: bytes read + written (both unique and total)
    3. Compute work: FLOPs
    4. Arithmetic intensity: FLOPs / bytes = how much compute per memory access
    5. Dependency chain: longest serial chain of dependent instructions
    """

    name: str = "unnamed_kernel"

    # === Launch configuration ===
    # INTERVIEW: "How do grid and block dimensions affect performance?"
    # Block size determines: occupancy, shared memory usage, synchronization scope
    # Grid size determines: total parallelism, tail effects
    grid_x: int = 1
    grid_y: int = 1
    grid_z: int = 1
    block_x: int = 256
    block_y: int = 1
    block_z: int = 1

    # === Per-thread resource usage ===
    registers_per_thread: int = 32      # From nvcc --ptxas-options=-v
    shared_memory_per_block: int = 0    # bytes (static + dynamic)

    # === Memory traffic ===
    # "Unique bytes" = the actual data footprint (what would be loaded from DRAM
    # in the best case, with perfect caching)
    # "Total bytes" = actual memory transactions including redundant loads
    # (cache misses, uncoalesced accesses, etc.)
    #
    # INTERVIEW: "What's the difference between unique and total bytes?"
    # Total > unique means there's redundant memory traffic. Causes:
    #   - Uncoalesced accesses (threads in a warp access scattered addresses)
    #   - Cache thrashing (working set exceeds cache)
    #   - Replay due to bank conflicts
    bytes_read_unique: int = 0          # bytes
    bytes_written_unique: int = 0       # bytes
    bytes_read_total: int = 0           # bytes (>= unique, accounts for waste)
    bytes_written_total: int = 0        # bytes

    # === Compute work ===
    flops: int = 0                      # total FP32 floating-point operations
    int_ops: int = 0                    # integer operations (address calc, etc.)

    # === Dependency chain ===
    # The longest serial chain of dependent instructions (in cycles).
    # This limits performance even with infinite parallelism — you can't
    # start the next instruction until the previous one finishes.
    # For simple kernels (vector add): 0 (fully parallel)
    # For reductions: log2(N) steps
    dependency_chain_cycles: int = 0

    @property
    def total_threads(self) -> int:
        return self.grid_x * self.grid_y * self.grid_z * self.threads_per_block

    @property
    def total_blocks(self) -> int:
        return self.grid_x * self.grid_y * self.grid_z

    @property
    def threads_per_block(self) -> int:
        return self.block_x * self.block_y * self.block_z

    @property
    def warps_per_block(self) -> int:
        """Number of warps per block (rounded up)."""
        return (self.threads_per_block + 31) // 32

    @property
    def total_bytes(self) -> int:
        """Total memory traffic (read + write)."""
        r = self.bytes_read_total if self.bytes_read_total > 0 else self.bytes_read_unique
        w = self.bytes_written_total if self.bytes_written_total > 0 else self.bytes_written_unique
        return r + w

    @property
    def unique_bytes(self) -> int:
        """Unique memory footprint."""
        return self.bytes_read_unique + self.bytes_written_unique

    @property
    def arithmetic_intensity(self) -> float:
        """
        Arithmetic Intensity = FLOPs / Bytes.

        INTERVIEW: "What is arithmetic intensity and why does it matter?"
        It determines whether a kernel is memory-bound or compute-bound.
        AI < ridge_point → memory-bound (most kernels)
        AI > ridge_point → compute-bound
        """
        if self.total_bytes == 0:
            return float('inf')
        return self.flops / self.total_bytes

    def summary(self) -> str:
        lines = [
            f"=== Kernel: {self.name} ===",
            f"  Grid: ({self.grid_x}, {self.grid_y}, {self.grid_z})",
            f"  Block: ({self.block_x}, {self.block_y}, {self.block_z}) "
            f"= {self.threads_per_block} threads, {self.warps_per_block} warps",
            f"  Total threads: {self.total_threads:,}",
            f"  Registers/thread: {self.registers_per_thread}",
            f"  Shared memory/block: {self.shared_memory_per_block:,} bytes",
            f"  Memory: {self.total_bytes:,} bytes "
            f"(read: {self.bytes_read_unique:,}, write: {self.bytes_written_unique:,})",
            f"  FLOPs: {self.flops:,}",
            f"  Arithmetic Intensity: {self.arithmetic_intensity:.2f} FLOP/byte",
        ]
        return "\n".join(lines)
