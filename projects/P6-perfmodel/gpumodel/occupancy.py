"""
occupancy.py — GPU Occupancy Calculator
=========================================

Computes the number of blocks/warps that can run concurrently on a single SM,
given a kernel's resource usage and the SM's limits.

This is one of the MOST COMMON interview questions for GPU roles:
"Walk me through how occupancy is calculated."

OCCUPANCY = achieved_warps / max_warps_per_SM

It is limited by FOUR resources (the binding constraint is whichever
allows the fewest blocks):
  1. Registers per SM: total_regs_used = regs_per_thread * threads_per_block * blocks
  2. Shared memory per SM: total_shared = shared_per_block * blocks
  3. Max blocks per SM: hardware limit (e.g., 32)
  4. Max warps per SM: hardware limit (e.g., 64) → max_blocks_from_warps = max_warps / warps_per_block

The achieved blocks = min(limit_from_regs, limit_from_shared, limit_from_blocks, limit_from_warps)
"""

from dataclasses import dataclass
from .machine import MachineModel
from .kernel import KernelDescriptor
import math


@dataclass
class OccupancyResult:
    """Result of an occupancy calculation."""
    blocks_per_sm: int          # Number of concurrent blocks per SM
    warps_per_sm: int           # Number of concurrent warps per SM
    occupancy: float            # Fraction of max warps (0.0 to 1.0)
    binding_constraint: str     # Which resource is the bottleneck

    # Individual limits (for understanding)
    limit_from_registers: int   # Max blocks from register file
    limit_from_shared_mem: int  # Max blocks from shared memory
    limit_from_block_limit: int # Max blocks from hardware block limit
    limit_from_warp_limit: int  # Max blocks from warp limit

    def summary(self) -> str:
        return (
            f"  Occupancy: {self.occupancy*100:.1f}% "
            f"({self.warps_per_sm}/{self.warps_per_sm + (64 - self.warps_per_sm)} warps, "
            f"{self.blocks_per_sm} blocks)\n"
            f"  Binding constraint: {self.binding_constraint}\n"
            f"  Limits — registers: {self.limit_from_registers} blocks, "
            f"shared mem: {self.limit_from_shared_mem} blocks, "
            f"block limit: {self.limit_from_block_limit}, "
            f"warp limit: {self.limit_from_warp_limit}"
        )


def calculate_occupancy(machine: MachineModel, kernel: KernelDescriptor) -> OccupancyResult:
    """
    Calculate occupancy for a kernel on a given GPU.

    INTERVIEW WALKTHROUGH (be able to reproduce on a whiteboard):

    Example: kernel with 256 threads/block, 32 regs/thread, 8KB shared,
             on a GPU with 65536 regs, 96KB shared, 64 max warps, 32 max blocks.

    Step 1: Warps per block = ceil(256 / 32) = 8 warps
    Step 2: Registers per block = 32 * 256 = 8192
            Blocks from regs = floor(65536 / 8192) = 8
    Step 3: Blocks from shared = floor(96 * 1024 / 8192) = 12
    Step 4: Blocks from block limit = 32
    Step 5: Blocks from warp limit = floor(64 / 8) = 8
    Step 6: Achieved blocks = min(8, 12, 32, 8) = 8
    Step 7: Achieved warps = 8 * 8 = 64
    Step 8: Occupancy = 64 / 64 = 100%
    Binding: registers AND warps (tied at 8 blocks)
    """

    warps_per_block = kernel.warps_per_block
    threads_per_block = kernel.threads_per_block

    if warps_per_block == 0 or threads_per_block == 0:
        return OccupancyResult(0, 0, 0.0, "zero_threads",
                               0, 0, 0, 0)

    # ─── Limit 1: Registers ──────────────────────────────────────────
    # Total registers used by one block = regs_per_thread * threads_per_block
    # (In practice, registers are allocated in granularity of warp-level
    # allocation units, but we simplify here.)
    #
    # INTERVIEW: "What happens if you use too many registers?"
    # Occupancy drops → fewer warps to hide latency → performance cliff.
    # This is called "register pressure." Solutions: reduce register usage
    # (compiler flags, code changes) or accept lower occupancy.
    regs_per_block = kernel.registers_per_thread * threads_per_block

    if regs_per_block > 0:
        limit_regs = machine.register_file_size // regs_per_block
    else:
        limit_regs = machine.max_blocks_per_sm

    # ─── Limit 2: Shared Memory ──────────────────────────────────────
    # INTERVIEW: "What is shared memory and how does it affect occupancy?"
    # Shared memory is fast on-chip SRAM shared by all threads in a block.
    # More shared memory per block → fewer blocks fit on the SM.
    shared_per_block = kernel.shared_memory_per_block
    if shared_per_block > 0:
        limit_shared = machine.shared_memory_per_sm // shared_per_block
    else:
        limit_shared = machine.max_blocks_per_sm

    # ─── Limit 3: Block Limit ────────────────────────────────────────
    limit_blocks = machine.max_blocks_per_sm

    # ─── Limit 4: Warp Limit ─────────────────────────────────────────
    limit_warps = machine.max_warps_per_sm // warps_per_block

    # ─── Achieved Blocks = min of all limits ─────────────────────────
    achieved_blocks = min(limit_regs, limit_shared, limit_blocks, limit_warps)
    achieved_blocks = max(0, achieved_blocks)  # can't be negative
    achieved_warps = achieved_blocks * warps_per_block

    # Determine binding constraint
    mins = {
        'registers': limit_regs,
        'shared_memory': limit_shared,
        'block_limit': limit_blocks,
        'warp_limit': limit_warps,
    }
    binding = min(mins, key=mins.get)

    # Occupancy as fraction of max warps
    occupancy = achieved_warps / machine.max_warps_per_sm if machine.max_warps_per_sm > 0 else 0.0

    return OccupancyResult(
        blocks_per_sm=achieved_blocks,
        warps_per_sm=achieved_warps,
        occupancy=occupancy,
        binding_constraint=binding,
        limit_from_registers=limit_regs,
        limit_from_shared_mem=limit_shared,
        limit_from_block_limit=limit_blocks,
        limit_from_warp_limit=limit_warps,
    )
