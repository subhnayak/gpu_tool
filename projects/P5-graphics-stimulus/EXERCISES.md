# Exercises — P5 Graphics Stimulus

Graded from foundational to advanced. Each builds on the project.

---

## Exercise 1: Add a Stencil Buffer
Add a `stencil` buffer (uint8) to `Framebuffer`. Implement stencil test and stencil operations (KEEP, ZERO, REPLACE, INCREMENT, DECREMENT) in the pipeline. Use it to implement a simple stencil shadow volume.
**What this teaches:** How stencil tests work in hardware (same unit as depth test), and the classic shadow volume algorithm.

## Exercise 2: Implement MSAA (Multi-Sample Anti-Aliasing)
Add N sample points per pixel (2x, 4x, 8x) at jittered positions. Evaluate coverage per sample, but run the fragment shader only once per pixel (at the centroid). Implement the resolve pass (average covered samples). Observe: coverage rate ≠ shading rate.
**What this teaches:** The difference between coverage and shading — fundamental to understanding Variable Rate Shading (VRS) and MSAA hardware.

## Exercise 3: Implement Early-Z and Measure Savings
Add a counter for "fragments skipped by early-Z" vs "total fragments." Render a complex scene with front-to-back ordering and report the savings. Then render back-to-front and compare.
**What this teaches:** Why draw-call sorting matters, and what the early-Z hardware optimization actually does.

## Exercise 4: Implement Hierarchical Z (Hi-Z)
Maintain a coarse depth buffer (e.g., 8x8 tile max-depth). Before rasterizing a triangle over a tile, test the triangle's minimum depth against the tile's maximum. Skip the tile if the triangle is entirely behind existing geometry.
**What this teaches:** How modern GPUs accelerate depth testing. Hi-Z is one of the most important GPU performance features.

## Exercise 5: Add a Compute-Shader-Style Pass
Implement a "dispatch" function that runs a callable over a 2D grid (like a compute shader). Use it to implement a post-process effect (e.g., box blur, edge detection using Sobel). Observe: no triangle setup, no rasterization — just a grid of threads.
**What this teaches:** The compute pipeline, and why it's a separate path from graphics.

## Exercise 6: Alpha Blending and Order Dependence
Render the same set of translucent triangles in two different orders. Compare the output images. Implement a simple sorted-triangle solution. Then implement weighted-blended OIT and compare.
**What this teaches:** Why order-independent transparency is hard, and the tradeoffs of different approaches.

## Exercise 7: Implement Anisotropic Filtering
When the texture footprint is elongated (one derivative much larger than the other), sample multiple texels along the long axis instead of just picking a single mip level. Compare visual quality against isotropic filtering.
**What this teaches:** Why anisotropic filtering exists (grazing-angle textures), and why it's more expensive.

## Exercise 8: Tile-Based / Binned Rasterizer
Instead of rasterizing each triangle over the whole framebuffer, first BIN triangles into screen-space tiles (e.g., 32x32). Then process each tile independently, loading only the relevant triangles. Discuss: why does this help on mobile GPUs?
**What this teaches:** Tile-based deferred rendering (TBDR) as used by ARM Mali, Apple, and Imagination GPUs. Reduces memory bandwidth by keeping the tile's color/depth in on-chip SRAM.

## Exercise 9: Reversed-Z
Implement the reversed-Z projection (near→1.0, far→0.0) with a GREATER depth test. Render a scene with far geometry and compare depth precision against standard projection. Use the depth visualization shader to see the difference.
**What this teaches:** Why reversed-Z dramatically improves depth precision for distant objects, and why it's now standard practice.

## Exercise 10: Wireframe and Overdraw Visualization
Add a wireframe rendering mode (rasterize only triangle edges). Add an overdraw heat map: instead of writing colors, increment a counter per pixel and color-map the result. This shows which pixels are shaded multiple times.
**What this teaches:** Debug visualization techniques used by GPU profiling tools (RenderDoc, Nsight).

## Exercise 11: Parallelize Triangle Binning — Then Fix Determinism
Parallelize the triangle-binning step from Exercise 8 using `std::thread`. Run the full regression suite and observe that hashes CHANGE (non-deterministic triangle order within tiles). Then fix it by sorting triangles within each tile by their original draw-order index. Verify hashes are restored.
**What this teaches:** The fundamental tension between parallelism and determinism, and how GPU verification teams handle it.

## Exercise 12: Triangle Fuzzer
Write a fuzzer that generates random triangles (random positions, including degenerate and near-degenerate cases, random depths, random attributes). Render them and assert: (a) no crashes, (b) stable hash when re-rendered. Run with thousands of iterations.
**What this teaches:** Fuzz testing for GPU correctness — verification teams do exactly this to find hardware corner-case bugs.
