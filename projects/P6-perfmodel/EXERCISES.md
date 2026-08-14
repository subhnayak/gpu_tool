# Exercises — P6 GPU Performance Model

Graded from foundational to advanced. Each builds on the model.

---

## Exercise 1: Model Before Coding
Before writing a new CUDA kernel, use the model to predict its performance. Then implement the kernel, measure, and compare. Document what you predicted, what you got, and why they differed.
**What this teaches:** Using performance models as a design tool, not just an analysis tool.

## Exercise 2: L2 Hit Rate Extension
Extend the model to account for L2 cache hit rate. Add a `l2_hit_rate` parameter (0.0 to 1.0) to `KernelDescriptor`. Effective bandwidth becomes: `effective_BW = L2_BW * hit_rate + DRAM_BW * (1 - hit_rate)`. Test with kernels that have varying working set sizes.
**What this teaches:** Cache effects are the #1 source of model error. Even a simple hit-rate model improves predictions significantly.

## Exercise 3: Bank Conflict Model
Add a model for shared memory bank conflicts. When `N` threads in a warp access the same bank, throughput drops by factor `N`. Add a `bank_conflict_factor` (1.0 = no conflicts, 32.0 = worst case). Predict the effect on matrix transpose with and without +1 padding.
**What this teaches:** Bank conflicts are a key shared-memory performance pitfall.

## Exercise 4: Launch Overhead Crossover
Find the kernel size (number of elements) where launch overhead is no longer negligible (>10% of total time). Plot predicted time vs N. Where is the crossover? What does this tell you about when to fuse kernels?
**What this teaches:** Why kernel fusion matters for small workloads.

## Exercise 5: Multi-Kernel Pipeline
Model a pipeline of 3 kernels with overlapping execution on streams. Predict total time with and without overlap. Account for PCIe transfer of input/output data.
**What this teaches:** Streaming and overlap are key to end-to-end performance.

## Exercise 6: Cache Model with Working Set
Add a simple cache model: if `working_set < L2_size`, use L2 bandwidth (typically 2-4x DRAM). If `working_set < L1_size`, use L1 bandwidth (~10x DRAM). Adjust the model and test with kernels of varying data sizes.
**What this teaches:** The memory hierarchy is not just DRAM. Cache-friendly algorithms can dramatically outperform the roofline prediction.

## Exercise 7: Calibrate Efficiency Factors
Run several well-understood kernels (vector add, copy, SGEMM) on your actual GPU. Measure achieved bandwidth and FLOPs. Set `achievable_bandwidth_fraction` and `achievable_flops_fraction` to match. How much does this improve predictions for other kernels?
**What this teaches:** Empirical calibration is essential for practical models.

## Exercise 8: Tensor Core Throughput
Add tensor core throughput to the machine model (e.g., 312 TFLOPS for A100 TF32). Add a `use_tensor_cores` flag to the kernel descriptor. Predict SGEMM performance with and without tensor cores.
**What this teaches:** Specialized hardware units fundamentally change the roofline.

## Exercise 9: Hypothetical Architecture Report
Invent a plausible next-gen GPU: +50% SMs, +30% clock, 2x bandwidth, 2x L2. Run the model on the P3 kernels. Write a one-page report summarizing: which kernels benefit most, which don't, and what the architect should prioritize. This is exactly what GPU architects' reports look like.
**What this teaches:** How to communicate performance analysis to decision-makers.

## Exercise 10: Find Where the Model Breaks
Find a CUDA kernel where the model's prediction is >2x off. Explain why. Possible candidates: kernels with heavy warp divergence, atomic contention, irregular memory access patterns, or instruction cache pressure. Document the failure mode and propose how to extend the model.
**What this teaches:** The most important lesson — knowing the MODEL'S limitations is as valuable as the model itself.
