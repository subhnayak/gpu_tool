"""
test_model.py — Tests for the GPU Performance Model
=====================================================

Tests with hand-computed expected values. Each test includes the
arithmetic in comments so you can reproduce it on a whiteboard.

Run: pytest tests/test_model.py -v
"""

import sys
import os
import math
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gpumodel.machine import MachineModel
from gpumodel.kernel import KernelDescriptor
from gpumodel.occupancy import calculate_occupancy
from gpumodel.roofline import classify_kernel, predicted_gflops
from gpumodel.model import predict


# ═══════════════════════════════════════════════════════════════════════
# A simple test machine with round numbers for easy hand-computation
# ═══════════════════════════════════════════════════════════════════════
def test_machine():
    """A GPU with easy-to-compute specs for testing."""
    return MachineModel(
        name="TestGPU",
        sm_count=10,
        clock_ghz=1.0,
        fp32_cores_per_sm=100,  # 10 SMs * 100 cores * 1GHz * 2(FMA) = 2000 GFLOPS peak
        max_warps_per_sm=64,
        max_threads_per_sm=2048,
        max_blocks_per_sm=16,
        register_file_size=65536,  # 64K registers per SM
        shared_memory_per_sm=49152,  # 48 KB
        max_shared_memory_per_block=49152,
        memory_bus_width_bits=256,
        memory_clock_ghz=1.0,  # 256 * 1.0 * 2 / 8 = 64 GB/s peak
        achievable_bandwidth_fraction=1.0,  # 100% for easy math
        achievable_flops_fraction=1.0,      # 100% for easy math
    )


# ═══════════════════════════════════════════════════════════════════════
# TEST: Occupancy Calculator
# ═══════════════════════════════════════════════════════════════════════

class TestOccupancy:
    """
    INTERVIEW: "Calculate the occupancy for a kernel with X threads,
    Y registers, Z shared memory on GPU with ..."
    (These are worked examples with the arithmetic shown.)
    """

    def test_basic_occupancy(self):
        """
        WORKED EXAMPLE 1:
          Block: 256 threads = 8 warps
          Registers: 32/thread → 32 * 256 = 8192 regs/block
          Shared: 0
          SM limits: 64 warps, 16 blocks, 65536 regs, 48KB shared

          Limit from regs: floor(65536 / 8192) = 8 blocks
          Limit from shared: 16 (no shared → hardware limit)
          Limit from blocks: 16
          Limit from warps: floor(64 / 8) = 8
          Achieved: min(8, 16, 16, 8) = 8 blocks = 64 warps = 100%
        """
        m = test_machine()
        k = KernelDescriptor(block_x=256, registers_per_thread=32)
        result = calculate_occupancy(m, k)

        assert result.blocks_per_sm == 8
        assert result.warps_per_sm == 64
        assert abs(result.occupancy - 1.0) < 0.01  # 100%

    def test_register_limited(self):
        """
        WORKED EXAMPLE 2:
          Block: 256 threads = 8 warps
          Registers: 128/thread → 128 * 256 = 32768 regs/block
          Shared: 0

          Limit from regs: floor(65536 / 32768) = 2 blocks
          Limit from warps: floor(64 / 8) = 8
          Achieved: min(2, ...) = 2 blocks = 16 warps = 25%
          Binding: registers
        """
        m = test_machine()
        k = KernelDescriptor(block_x=256, registers_per_thread=128)
        result = calculate_occupancy(m, k)

        assert result.blocks_per_sm == 2
        assert result.warps_per_sm == 16
        assert abs(result.occupancy - 0.25) < 0.01  # 25%
        assert result.binding_constraint == "registers"

    def test_shared_memory_limited(self):
        """
        WORKED EXAMPLE 3:
          Block: 128 threads = 4 warps
          Registers: 16/thread → 2048 regs/block
          Shared: 24576 bytes (24KB) per block

          Limit from regs: floor(65536 / 2048) = 32 → capped by block limit = 16
          Limit from shared: floor(49152 / 24576) = 2 blocks
          Limit from warps: floor(64 / 4) = 16
          Achieved: min(16, 2, 16, 16) = 2 blocks = 8 warps = 12.5%
          Binding: shared_memory
        """
        m = test_machine()
        k = KernelDescriptor(block_x=128, registers_per_thread=16,
                             shared_memory_per_block=24576)
        result = calculate_occupancy(m, k)

        assert result.blocks_per_sm == 2
        assert result.binding_constraint == "shared_memory"


# ═══════════════════════════════════════════════════════════════════════
# TEST: Roofline Classification
# ═══════════════════════════════════════════════════════════════════════

class TestRoofline:

    def test_memory_bound_classification(self):
        """
        TestGPU: peak = 2000 GFLOPS, BW = 64 GB/s, ridge = 2000/64 = 31.25 FLOP/byte
        Kernel with AI = 0.1 → definitely memory-bound
        """
        m = test_machine()
        k = KernelDescriptor(
            bytes_read_unique=1000000, bytes_written_unique=0,
            flops=100000  # AI = 100000/1000000 = 0.1
        )
        assert classify_kernel(m, k) == "memory-bound"

    def test_compute_bound_classification(self):
        """
        Kernel with AI = 100 → definitely compute-bound (ridge = 31.25)
        """
        m = test_machine()
        k = KernelDescriptor(
            bytes_read_unique=1000, bytes_written_unique=0,
            flops=100000  # AI = 100000/1000 = 100
        )
        assert classify_kernel(m, k) == "compute-bound"


# ═══════════════════════════════════════════════════════════════════════
# TEST: Model Monotonicity
# ═══════════════════════════════════════════════════════════════════════

class TestMonotonicity:
    """
    Sanity checks: more resources should never predict SLOWER performance.
    """

    def test_more_bandwidth_not_slower(self):
        """Doubling bandwidth should not increase predicted time."""
        m1 = test_machine()
        m2 = test_machine()
        m2.memory_clock_ghz *= 2  # double BW

        k = KernelDescriptor(
            name="bw_test", grid_x=1024, block_x=256,
            registers_per_thread=32,
            bytes_read_unique=1 << 20, bytes_written_unique=1 << 20,
            flops=1 << 15,
        )

        t1 = predict(m1, k).predicted_time
        t2 = predict(m2, k).predicted_time
        assert t2 <= t1 * 1.01, "More bandwidth should not predict slower time"

    def test_more_compute_not_slower(self):
        """Doubling compute should not increase predicted time."""
        m1 = test_machine()
        m2 = test_machine()
        m2.fp32_cores_per_sm *= 2

        k = KernelDescriptor(
            name="comp_test", grid_x=1024, block_x=256,
            registers_per_thread=32,
            bytes_read_unique=1 << 10, bytes_written_unique=0,
            flops=1 << 30,  # lots of compute
        )

        t1 = predict(m1, k).predicted_time
        t2 = predict(m2, k).predicted_time
        assert t2 <= t1 * 1.01, "More compute should not predict slower time"

    def test_more_sms_not_slower(self):
        """Adding SMs should not increase predicted time."""
        m1 = test_machine()
        m2 = test_machine()
        m2.sm_count = 20

        k = KernelDescriptor(
            name="sm_test", grid_x=2048, block_x=256,
            registers_per_thread=32,
            bytes_read_unique=1 << 20, bytes_written_unique=1 << 20,
            flops=1 << 20,
        )

        t1 = predict(m1, k).predicted_time
        t2 = predict(m2, k).predicted_time
        assert t2 <= t1 * 1.01, "More SMs should not predict slower time"


# ═══════════════════════════════════════════════════════════════════════
# TEST: Unit Consistency
# ═══════════════════════════════════════════════════════════════════════

class TestUnits:

    def test_peak_flops_units(self):
        """Verify peak GFLOPS computation: 10 * 100 * 1.0 * 2 = 2000 GFLOPS."""
        m = test_machine()
        assert abs(m.peak_fp32_gflops - 2000.0) < 0.1

    def test_peak_bandwidth_units(self):
        """Verify peak bandwidth: 256 * 1.0 * 2 / 8 = 64 GB/s."""
        m = test_machine()
        assert abs(m.peak_bandwidth_gb_s - 64.0) < 0.1

    def test_ridge_point(self):
        """Ridge = 2000 / 64 = 31.25 FLOP/byte."""
        m = test_machine()
        assert abs(m.ridge_point - 31.25) < 0.1

    def test_arithmetic_intensity(self):
        """AI = flops / total_bytes = 1000 / 100 = 10."""
        k = KernelDescriptor(
            bytes_read_unique=80, bytes_written_unique=20,
            flops=1000
        )
        assert abs(k.arithmetic_intensity - 10.0) < 0.01


# ═══════════════════════════════════════════════════════════════════════
# Run tests directly (without pytest)
# ═══════════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    print("Running tests...")
    passed = 0
    failed = 0

    for cls in [TestOccupancy, TestRoofline, TestMonotonicity, TestUnits]:
        obj = cls()
        for name in dir(obj):
            if name.startswith("test_"):
                try:
                    getattr(obj, name)()
                    print(f"  PASS: {cls.__name__}.{name}")
                    passed += 1
                except Exception as e:
                    print(f"  FAIL: {cls.__name__}.{name}: {e}")
                    failed += 1

    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
