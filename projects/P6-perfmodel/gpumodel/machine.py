"""
machine.py — GPU Machine Model
================================

Describes the hardware capabilities of a GPU: SM count, clock,
memory bandwidth, register file, shared memory, etc.

INTERVIEW: "Describe the architecture of an NVIDIA GPU."
- Multiple Streaming Multiprocessors (SMs)
- Each SM has: CUDA cores, register file, shared memory, L1 cache, warp schedulers
- SMs share: L2 cache, memory controllers, global memory (DRAM)
- Peak FP32 FLOPs = SM_count * cores_per_SM * clock * 2 (FMA = 2 ops)
- Peak bandwidth = memory_bus_width * memory_clock * 2 (DDR) / 8 (bits to bytes)
"""

from dataclasses import dataclass, field
from typing import Optional
import json
import re


@dataclass
class MachineModel:
    """
    GPU machine model. All values are for a SINGLE GPU.

    INTERVIEW: "What limits GPU performance?"
    Three walls:
      1. Compute bound: not enough FLOPs/s
      2. Memory bound: not enough bytes/s (bandwidth)
      3. Latency bound: not enough parallelism to hide memory latency

    The roofline model captures (1) and (2). Little's Law captures (3).
    """

    name: str = "Generic GPU"

    # === Streaming Multiprocessor configuration ===
    sm_count: int = 72              # Number of SMs
    clock_ghz: float = 1.5          # SM clock in GHz
    fp32_cores_per_sm: int = 64     # CUDA cores (FP32) per SM
    warp_size: int = 32             # Threads per warp (always 32 on NVIDIA)

    # === Per-SM resource limits (for occupancy calculation) ===
    # INTERVIEW: "What determines occupancy?"
    # The minimum of: registers, shared memory, warps, and blocks limits.
    max_warps_per_sm: int = 64          # Max concurrent warps per SM
    max_threads_per_sm: int = 2048      # Max concurrent threads per SM
    max_blocks_per_sm: int = 32         # Max concurrent blocks per SM
    register_file_size: int = 65536     # 32-bit registers per SM
    max_registers_per_thread: int = 255 # Hardware limit
    shared_memory_per_sm: int = 98304   # bytes (configurable L1/shared split)
    max_shared_memory_per_block: int = 49152  # bytes

    # === Memory hierarchy ===
    l1_size_kb: int = 128               # L1 cache per SM (KB)
    l2_size_kb: int = 4096              # L2 cache total (KB)

    # === Global memory ===
    memory_bus_width_bits: int = 384    # Memory bus width in bits
    memory_clock_ghz: float = 1.215    # Memory clock in GHz (effective)
    memory_type: str = "GDDR6X"

    # === Achievable fraction of peak ===
    # Real workloads never hit theoretical peak. These factors account for
    # overhead (instruction fetch, control flow, cache misses, etc.).
    # INTERVIEW: "What fraction of peak bandwidth can a kernel achieve?"
    # Typically 80-90% for well-optimized memory-bound kernels.
    # Compute-bound kernels: 60-80% of peak FLOPs.
    achievable_bandwidth_fraction: float = 0.85
    achievable_flops_fraction: float = 0.70

    # === Derived quantities ===
    @property
    def peak_fp32_tflops(self) -> float:
        """
        Peak FP32 throughput in TFLOPS.
        Formula: SM_count * cores_per_SM * clock_GHz * 2 (FMA counts as 2 ops) / 1000
        """
        return self.sm_count * self.fp32_cores_per_sm * self.clock_ghz * 2 / 1000

    @property
    def peak_fp32_gflops(self) -> float:
        """Peak FP32 throughput in GFLOPS."""
        return self.sm_count * self.fp32_cores_per_sm * self.clock_ghz * 2

    @property
    def peak_bandwidth_gb_s(self) -> float:
        """
        Peak memory bandwidth in GB/s.
        Formula: bus_width_bits * memory_clock_GHz * 2 (DDR) / 8 (bits→bytes)

        Note: GDDR6X uses PAM4 signaling (4 instead of 2), but the
        "effective clock" already accounts for this, so we use * 2 for DDR.
        Adjust if your GPU spec gives the raw clock.
        """
        return self.memory_bus_width_bits * self.memory_clock_ghz * 2 / 8

    @property
    def achievable_bandwidth_gb_s(self) -> float:
        """Achievable bandwidth = peak * efficiency factor."""
        return self.peak_bandwidth_gb_s * self.achievable_bandwidth_fraction

    @property
    def achievable_gflops(self) -> float:
        """Achievable FLOPs = peak * efficiency factor."""
        return self.peak_fp32_gflops * self.achievable_flops_fraction

    @property
    def ridge_point(self) -> float:
        """
        Roofline ridge point: arithmetic intensity where compute and
        memory ceilings intersect.
        AI_ridge = peak_GFLOPS / peak_BW_GB_s  (FLOP/byte)

        INTERVIEW: "What is the roofline ridge point?"
        Kernels with AI < ridge_point are memory-bound.
        Kernels with AI > ridge_point are compute-bound.
        """
        return self.achievable_gflops / self.achievable_bandwidth_gb_s

    def summary(self) -> str:
        """Print a human-readable machine summary."""
        lines = [
            f"=== GPU Machine Model: {self.name} ===",
            f"  SMs: {self.sm_count} × {self.fp32_cores_per_sm} FP32 cores @ {self.clock_ghz} GHz",
            f"  Peak FP32: {self.peak_fp32_tflops:.1f} TFLOPS ({self.peak_fp32_gflops:.0f} GFLOPS)",
            f"  Achievable FP32: {self.achievable_gflops:.0f} GFLOPS ({self.achievable_flops_fraction*100:.0f}%)",
            f"  Memory: {self.memory_type}, {self.memory_bus_width_bits}-bit @ {self.memory_clock_ghz} GHz",
            f"  Peak BW: {self.peak_bandwidth_gb_s:.0f} GB/s",
            f"  Achievable BW: {self.achievable_bandwidth_gb_s:.0f} GB/s ({self.achievable_bandwidth_fraction*100:.0f}%)",
            f"  Ridge point: {self.ridge_point:.1f} FLOP/byte",
            f"  Per-SM: {self.max_warps_per_sm} warps, {self.register_file_size} regs, "
            f"{self.shared_memory_per_sm//1024}KB shared, {self.max_blocks_per_sm} blocks",
            f"  L1: {self.l1_size_kb} KB/SM, L2: {self.l2_size_kb} KB total",
        ]
        return "\n".join(lines)

    @staticmethod
    def from_device_query(text: str) -> "MachineModel":
        """
        Parse output from CUDA deviceQuery (or similar) to build a machine model.
        This is approximate — deviceQuery doesn't report everything we need.

        Typical deviceQuery output includes:
          "Multiprocessors: 72"
          "CUDA Cores/MP: 64"
          "GPU Max Clock rate: 1530 MHz"
          "Memory Bus Width: 384-bit"
          "Memory Clock rate: 1215 Mhz"
          etc.
        """
        m = MachineModel(name="Parsed from deviceQuery")

        # Try to extract values with regex
        patterns = {
            'sm_count': r'(?:Multiprocessors|SM Count)[:\s]+(\d+)',
            'cores': r'(?:CUDA Cores/MP|Cores per SM)[:\s]+(\d+)',
            'clock': r'GPU (?:Max )?Clock rate[:\s]+(\d+)\s*MHz',
            'mem_bus': r'Memory Bus Width[:\s]+(\d+)',
            'mem_clock': r'Memory Clock rate[:\s]+(\d+)\s*MHz',
            'max_threads': r'Max threads per multiprocessor[:\s]+(\d+)',
            'max_blocks': r'Max (?:blocks|blocks per multiprocessor)[:\s]+(\d+)',
            'regs': r'(?:Total.*registers|Registers) per (?:block|multiprocessor)[:\s]+(\d+)',
            'shared': r'(?:Shared memory|Maximum shared memory) per (?:block|multiprocessor)[:\s]+(\d+)',
        }

        for key, pattern in patterns.items():
            match = re.search(pattern, text, re.IGNORECASE)
            if match:
                val = int(match.group(1))
                if key == 'sm_count': m.sm_count = val
                elif key == 'cores': m.fp32_cores_per_sm = val
                elif key == 'clock': m.clock_ghz = val / 1000.0
                elif key == 'mem_bus': m.memory_bus_width_bits = val
                elif key == 'mem_clock': m.memory_clock_ghz = val / 1000.0
                elif key == 'max_threads': m.max_threads_per_sm = val
                elif key == 'max_blocks': m.max_blocks_per_sm = val
                elif key == 'regs': m.register_file_size = val
                elif key == 'shared': m.max_shared_memory_per_block = val

        m.max_warps_per_sm = m.max_threads_per_sm // m.warp_size
        return m


# ═══════════════════════════════════════════════════════════════════════
# PRESET MACHINES
# ═══════════════════════════════════════════════════════════════════════
# These are APPROXIMATE. The reader should fill in their own GPU's specs.
# Sources: NVIDIA whitepapers, techpowerup.com, official specs.

def turing_rtx2080() -> MachineModel:
    """Approximate NVIDIA RTX 2080 (Turing, TU104)."""
    return MachineModel(
        name="RTX 2080 (Turing, approximate)",
        sm_count=46, fp32_cores_per_sm=64, clock_ghz=1.71,
        max_warps_per_sm=32, max_threads_per_sm=1024,
        max_blocks_per_sm=16, register_file_size=65536,
        shared_memory_per_sm=65536, max_shared_memory_per_block=49152,
        memory_bus_width_bits=256, memory_clock_ghz=1.75,
        memory_type="GDDR6",
        l1_size_kb=64, l2_size_kb=4096,
    )

def ampere_a100() -> MachineModel:
    """Approximate NVIDIA A100 (Ampere, GA100)."""
    return MachineModel(
        name="A100 (Ampere, approximate)",
        sm_count=108, fp32_cores_per_sm=64, clock_ghz=1.41,
        max_warps_per_sm=64, max_threads_per_sm=2048,
        max_blocks_per_sm=32, register_file_size=65536,
        shared_memory_per_sm=163840, max_shared_memory_per_block=163840,
        memory_bus_width_bits=5120, memory_clock_ghz=1.215,
        memory_type="HBM2e",
        l1_size_kb=192, l2_size_kb=40960,
    )

def ada_rtx4090() -> MachineModel:
    """Approximate NVIDIA RTX 4090 (Ada Lovelace, AD102)."""
    return MachineModel(
        name="RTX 4090 (Ada, approximate)",
        sm_count=128, fp32_cores_per_sm=128, clock_ghz=2.52,
        max_warps_per_sm=48, max_threads_per_sm=1536,
        max_blocks_per_sm=24, register_file_size=65536,
        shared_memory_per_sm=102400, max_shared_memory_per_block=102400,
        memory_bus_width_bits=384, memory_clock_ghz=1.313,
        memory_type="GDDR6X",
        l1_size_kb=128, l2_size_kb=73728,
    )
