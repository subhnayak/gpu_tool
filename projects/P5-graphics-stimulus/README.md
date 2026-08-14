# P5 — Software Rasterizer & Graphics Stimulus Generator

## Why a Software Rasterizer?

**Design rationale:** The highest-value, lowest-friction way to learn the graphics pipeline is to **implement** it, not to call it through an API. A software rasterizer:

1. **Teaches the pipeline** far better than calling OpenGL/Vulkan — you must understand every stage to implement it
2. **Is exactly what verification teams build** — a "golden model" reference rasterizer that produces bit-identical output, used to check hardware correctness
3. **Has zero dependencies** — runs anywhere, compiles with any C++ compiler
4. **Is fully deterministic** — unlike GPU output, which varies by vendor and driver
5. **Makes every pipeline stage individually testable** — the kind of testing GPU verification engineers do daily

The optional OpenGL path (`ENABLE_GL=ON`) exists for comparison, but the software rasterizer IS the project.

## Learning Objectives

- [ ] Implement and explain every stage of the graphics pipeline
- [ ] Understand fixed-function vs programmable stages
- [ ] Implement the top-left fill rule and explain why it exists
- [ ] Implement perspective-correct interpolation and explain why affine is wrong
- [ ] Implement 2x2 quad-based fragment processing with helper lanes
- [ ] Understand clipping (Sutherland-Hodgman) and why the near plane is special
- [ ] Produce deterministic output usable as verification stimulus
- [ ] Compare golden images with exact and tolerance-based methods

## Architecture

```
Vertex Buffer ──► Input Assembly ──► Vertex Shader ──► Clipping
                      │                    │               │
                 (index fetch)     (MVP transform)   (Sutherland-Hodgman)
                                                          │
                                                    Perspective Divide
                                                          │
                                                    Viewport Transform
                                                          │
                                                    Backface Culling
                                                          │
                                                    Triangle Setup
                                                          │
                                                    Rasterization
                                                    (edge function,
                                                     fill rule,
                                                     2x2 quads)
                                                          │
                                            ┌─────── Early-Z ───────┐
                                            │                       │
                                      Fragment Shader         (skip if
                                            │                  occluded)
                                      Depth/Stencil Test
                                            │
                                         Blending
                                            │
                                      Framebuffer Write
```

## Files

| File | What it teaches |
|------|----------------|
| `include/math3d.h` | Transform chain, projection matrices, GL vs VK conventions |
| `include/pipeline.h` | All pipeline stages, clipping, pipeline state |
| `include/framebuffer.h` | Framebuffer, CRC32 checksumming, PPM/BMP output |
| `include/texture.h` | Texture sampling, mipmaps, LOD from derivatives |
| `src/raster.cpp` | Edge-function rasterizer, fill rule, perspective-correct interp, quad shading |
| `src/shaders.cpp` | Example vertex/fragment shaders |
| `src/stimulus_main.cpp` | CLI stimulus generator with hardware corner-case scenes |
| `src/golden_check.cpp` | Golden image comparison (exact + tolerance) |
| `src/tests.cpp` | Correctness tests |

## Build & Run

```bash
mkdir build && cd build
cmake ..
cmake --build .

# Run tests
./tests          # or .\tests.exe on Windows

# List available stimulus scenes
./stimulus --list-scenes

# Run a specific scene
./stimulus run --scene 1 --out scene1.ppm

# Run all scenes and print golden hashes
./stimulus --all

# Hash-only mode (for CI regression checks)
./stimulus run --scene 2 --hash-only
```

## Acceptance Criteria

1. **Bit-identical output**: Running `stimulus --all` produces identical CRC32 hashes across multiple runs
2. **Optimization invariance**: Hashes are identical when compiled with `-O0` and `-O2` (with `/fp:strict` or `-ffp-contract=off`)
3. **Fill rule correctness**: The fill-rule test in `tests.cpp` passes — two adjacent triangles shade every shared pixel exactly once
4. **All 12 scenes render without crashes**, including degenerate triangles, near-plane clipping, and huge triangles
5. **All tests pass**: `./tests` returns exit code 0

## Interview Questions This Project Answers

| Question | Where to find the answer |
|----------|------------------------|
| Walk through the graphics pipeline stages | `pipeline.h` — each stage documented |
| What is the top-left fill rule and why? | `raster.cpp` — implemented and explained |
| How does perspective-correct interpolation work? | `raster.cpp` — the 1/w trick, explained |
| Why do GPUs shade in 2x2 quads? | `raster.cpp`, `texture.h` — helper lanes for derivatives |
| What's the difference between GL and Vulkan depth ranges? | `math3d.h` — both projections with comments |
| How does the GPU select texture mip level? | `texture.h` — LOD from screen-space derivatives |
| How do you verify GPU rendering correctness? | `golden_check.cpp`, `DETERMINISM.md` |
| What breaks rendering determinism? | `DETERMINISM.md` — comprehensive list |
| What is Sutherland-Hodgman clipping? | `pipeline.h` — full implementation |
| Why is the near plane special for clipping? | `pipeline.h` — comment on w < 0 |
