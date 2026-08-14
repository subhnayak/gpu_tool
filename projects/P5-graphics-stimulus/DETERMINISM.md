# Determinism in Rendering — What Makes It Work, What Breaks It

## Why Determinism Matters for GPU Verification

A verification team needs to compare the hardware's output against a known-correct reference. If the reference itself isn't deterministic, there's no ground truth. A software rasterizer MUST produce bit-identical output across:
- Multiple runs of the same binary
- Different optimization levels (-O0 vs -O2)
- Different compilers (ideally)
- Different machines (with the same floating-point behavior)

GPU hardware output, by contrast, is generally NOT deterministic across vendors or even driver versions. This is why verification teams maintain per-configuration golden images with tolerances.

## What Makes Our Software Rasterizer Deterministic

1. **Single-threaded execution**: Triangle processing order is fixed by the draw call order
2. **IEEE 754 compliance**: All operations are standard single-precision float
3. **No fast-math**: Compiled with `/fp:strict` (MSVC) or `-ffp-contract=off` (GCC/Clang)
4. **Deterministic fill rule**: Top-left rule assigns every shared-edge pixel to exactly one triangle
5. **Fixed evaluation order**: Expressions are evaluated left-to-right within each operation
6. **No uninitialized memory**: Framebuffer is explicitly cleared before use

## What Breaks Determinism

### 1. Floating-Point Reassociation (`-ffast-math`, `/fp:fast`)
The compiler may rearrange `(a + b) + c` to `a + (b + c)`. Due to floating-point rounding, these give different results. Example:
```
a = 1e20, b = -1e20, c = 1.0
(a + b) + c = 0 + 1 = 1.0          ← correct
a + (b + c) = 1e20 + (-1e20) = 0.0  ← wrong (c was lost)
```
**FIX**: Never use `-ffast-math`. Use `/fp:strict` or `-ffp-contract=off`.

### 2. FMA Contraction
A Fused Multiply-Add (FMA) computes `a*b + c` with a single rounding, while separate MUL+ADD has two roundings. The results differ by up to 0.5 ULP. Compilers may silently contract `a*b + c` into an FMA.
**FIX**: Use `-ffp-contract=off` to prevent automatic FMA contraction.

### 3. Different Rasterization Rules Between Vendors
- NVIDIA, AMD, and Intel GPUs implement subtly different rasterization rules
- The D3D spec allows ±0.5 pixel tolerance on triangle edges
- Vulkan's `VK_EXT_line_rasterization` exists because line rasterization differs
- Fill-rule corner cases (vertices exactly on pixel centers) may differ
**FIX**: Per-vendor golden images with per-pixel tolerance masks.

### 4. Undefined Behavior in Shaders
- Out-of-bounds array access in shaders → undefined results (not a crash on GPUs!)
- Integer overflow in shaders → implementation-defined
- NaN/Inf propagation rules differ between vendors
**FIX**: Shader validation, careful testing.

### 5. Uninitialized Framebuffer Contents
- Not clearing the framebuffer before rendering → stale data from previous frames or allocations
- Some GPUs zero framebuffer memory, others don't (security vs performance)
**FIX**: Always clear explicitly.

### 6. Multithreaded Triangle Ordering
- If triangles are binned to tiles and processed in parallel, the order of blending/depth-test operations becomes non-deterministic
- This can cause per-pixel color differences when triangles overlap
**FIX**: Deterministic tile assignment and ordering. Or: sort triangles before rasterization.

### 7. Transcendental Function Implementations
- `sin()`, `cos()`, `exp()`, `sqrt()` may differ between CPU and GPU, and between vendors
- The GPU typically uses lower-precision approximations
**FIX**: Use only operations with exact IEEE 754 semantics for golden-model work.

### 8. Texture Filtering Edge Cases
- Bilinear filtering at texture borders with different wrap modes
- Mipmap level selection is implementation-defined within a range
**FIX**: Define exact filtering behavior in the reference rasterizer.

## How a Verification Team Handles This

1. **Software reference rasterizer** (this project) defines the ground truth
2. **Per-test tolerances**: Each test specifies allowed max pixel diff and channel diff
3. **Masked regions**: Known-different pixels (e.g., triangle edges) are excluded
4. **Statistical metrics**: PSNR, SSIM, percentage of differing pixels
5. **Per-vendor/per-config golden images**: Separate baselines for each GPU + driver
6. **Invariance testing**: Same scene rendered twice on the SAME hardware must be identical
7. **Triage process**: New mismatches are investigated as potential hardware bugs

## Interview-Relevant Takeaways

- **Know the IEEE 754 flags**: strict vs fast, FMA contraction, reassociation
- **Know the fill rule**: top-left, and WHY (no gaps, no overdraw)
- **Know what `invariant` means** in GLSL: same shader + same inputs = same output
- **Know that cross-vendor invariance doesn't exist** in any graphics API
- **Know how verification teams deal with this**: tolerances, per-vendor goldens, triage
