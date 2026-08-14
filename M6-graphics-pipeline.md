# Module M6 — The Graphics Pipeline

## Why This Matters for This Role

You are interviewing for a team that builds a **stimulus development framework** — think of it as a software-side harness that exercises GPU hardware the way OpenGL and CUDA exercise it, but aimed at verification rather than shipping frames. That means you need to:

1. **Know every stage of the graphics pipeline** — because each stage is a test surface. If you can't explain what the tessellator does, you can't write a test that stresses its corner cases.
2. **Understand how the logical pipeline maps to silicon** — because the bugs you're hunting live in the hardware, not in API abstractions.
3. **Know why the APIs differ** — because the framework likely exposes both GL-style and Vulkan-style interfaces, and you'll need to reason about which state combinations are legal.
4. **Understand compute and graphics on the same hardware** — because the same SMs run vertex shaders, fragment shaders, and CUDA kernels, and your tests must cover all paths.

This module goes deep on mechanism and hardware mapping, not on how to ship a pretty renderer.

---

## 1. The Logical Pipeline End to End

Here is the full pipeline as a block diagram. Memorize this — you will draw it on a whiteboard.

```
                        ┌─────────────────────────────────────────────────────────┐
                        │                  APPLICATION / CPU                      │
                        │   (sets state, binds buffers, issues draw calls)        │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Draw command
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  INPUT ASSEMBLY (IA)          [Fixed-function]           │
                        │  Reads vertex buffers, index buffers.                    │
                        │  Assembles vertices into primitives (triangles, etc.)    │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Assembled vertices
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  VERTEX SHADER (VS)           [Programmable]            │
                        │  Per-vertex transform. Outputs clip-space position +    │
                        │  varyings (color, UV, normal, etc.)                     │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Transformed vertices
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  TESSELLATION (optional)                                │
                        │  ┌─ Hull / TCS  [Programmable] ──────────────────────┐  │
                        │  │  Outputs control points + tessellation levels     │  │
                        │  └───────────────────┬───────────────────────────────┘  │
                        │  ┌───────────────────▼───────────────────────────────┐  │
                        │  │  Tessellator      [Fixed-function]                │  │
                        │  │  Generates new vertices per patch                 │  │
                        │  └───────────────────┬───────────────────────────────┘  │
                        │  ┌───────────────────▼───────────────────────────────┐  │
                        │  │  Domain / TES     [Programmable]                  │  │
                        │  │  Positions each generated vertex                  │  │
                        │  └───────────────────────────────────────────────────┘  │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  GEOMETRY SHADER (GS) (optional) [Programmable]         │
                        │  Can amplify/discard primitives.                         │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  TRANSFORM FEEDBACK / STREAM-OUT  [Fixed-function ctrl] │
                        │  Optionally captures post-GS vertices to buffer.        │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Primitives in clip space
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  CLIPPING               [Fixed-function]                │
                        │  Clips to view frustum (6 planes + user clip planes).   │
                        │  May generate new vertices at clip boundaries.          │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  PERSPECTIVE DIVIDE      [Fixed-function]               │
                        │  (x,y,z,w) → (x/w, y/w, z/w) = NDC                    │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  VIEWPORT TRANSFORM      [Fixed-function]               │
                        │  NDC → window coordinates (pixel x,y + depth)           │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  BACKFACE CULLING         [Fixed-function]              │
                        │  Discards triangles facing away from camera.            │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  RASTERIZATION            [Fixed-function]              │
                        │  Determines which pixels/samples a primitive covers.    │
                        │  Generates fragments with interpolated attributes.      │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Fragments (with interpolated varyings)
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  EARLY DEPTH/STENCIL      [Fixed-function]             │
                        │  Kills fragments that fail depth/stencil BEFORE shader. │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Surviving fragments
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  FRAGMENT / PIXEL SHADER  [Programmable]               │
                        │  Computes color (and optionally new depth).             │
                        └──────────────────────┬──────────────────────────────────┘
                                               │ Shaded fragments
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  LATE DEPTH/STENCIL       [Fixed-function]             │
                        │  For fragments that modified depth or used discard.     │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  BLENDING / COLOR WRITE   [Fixed-function]             │
                        │  Combines fragment color with framebuffer.              │
                        └──────────────────────┬──────────────────────────────────┘
                                               │
                                               ▼
                        ┌──────────────────────────────────────────────────────────┐
                        │  ROP → FRAMEBUFFER WRITE  [Fixed-function]             │
                        │  Final color/depth written to memory.                   │
                        └──────────────────────────────────────────────────────────┘
```

### 1.1 Input Assembly (IA)

**What it does:** Reads raw vertex data from GPU memory (vertex buffers), optionally using an index buffer to avoid duplicating vertices. Assembles vertices into primitives based on the selected topology: point list, line list/strip, triangle list/strip/fan, patch list (for tessellation), and adjacency variants.

**Programmable vs fixed-function:** Fixed-function. The application configures it (buffer bindings, format, stride, topology), but the stage itself is hardware logic.

**Data flow:**
- **In:** Vertex buffer(s) + optional index buffer + draw command parameters (count, first, instance count)
- **Out:** Individual vertices with their attributes, grouped into primitives

**Instancing:** The IA can combine per-vertex attributes with per-instance attributes. The hardware uses `gl_InstanceID`/`SV_InstanceID` and instance divisors to step through instance data. This lets you draw 1000 trees with one draw call.

**Why it exists:** Decouples data layout from processing. The GPU needs a fixed contract for how to fetch data before the shader can run.

### 1.2 Vertex Shader (VS)

**What it does:** Runs once per vertex (after IA). Typically applies the model-view-projection transform to produce a clip-space position. Also computes per-vertex outputs (varyings) that will be interpolated across triangles: UVs, normals, colors, tangents.

**Programmable:** Yes — this is a shader you write.

**Data flow:**
- **In:** One vertex's attributes (position, normal, UV, etc.) plus uniforms/constants (MVP matrix, etc.)
- **Out:** `gl_Position` (clip-space vec4) + user-defined varyings

**Key point:** The vertex shader output position is in **clip space** (homogeneous coordinates, before the divide by w). This is critical for clipping.

### 1.3 Tessellation

Three sub-stages, only active when a tessellation shader is bound:

**Hull Shader / Tessellation Control Shader (TCS)** — Programmable. Receives a patch (a set of control points, e.g., 16 for a bicubic patch). Outputs modified control points plus **tessellation levels** (inner and outer) that tell the tessellator how finely to subdivide.

**Tessellator** — Fixed-function. Takes the tessellation levels and a domain type (triangle, quad, isoline) and generates a set of (u,v) or (u,v,w) coordinates within the patch, plus connectivity.

**Domain Shader / Tessellation Evaluation Shader (TES)** — Programmable. Runs once per generated vertex. Receives the (u,v) coordinate and the control points. Evaluates the surface equation (e.g., Bézier) to produce a final position and varyings.

**Why it exists:** LOD control. You send coarse patches and let the GPU add detail near the camera. Important for terrain, CAD, and film-quality surfaces.

### 1.4 Geometry Shader (GS)

**What it does:** Receives a complete primitive (triangle + optional adjacency) and can output zero or more primitives. Can change topology (input triangles, output lines). Can amplify geometry.

**Programmable:** Yes.

**Why it's rarely used in production:** It serializes work — one GS invocation must complete before the next primitive's output is known, which kills parallelism. Mesh shaders are the modern replacement.

**Still matters for verification:** The GS is a real hardware path that must be tested.

### 1.5 Transform Feedback / Stream-Out

**What it does:** Captures post-vertex-processing (post-VS or post-GS) vertex data into a buffer object. Can be used for GPU-side particle systems or multi-pass geometry processing.

**Fixed-function control:** The app specifies which varyings to capture and where to write them. No shader involved at this stage.

### 1.6 Clipping

**What it does:** Tests each primitive against the **clip volume**. In homogeneous clip space the clip volume is defined by:

```
-w <= x <= w
-w <= y <= w
z_near <= z <= w        (z_near = -w for GL, 0 for Vulkan/D3D)
```

Vertices outside are clipped. Triangles partially outside are split into smaller triangles that lie within the volume. User clip planes (up to 8 in GL) can add additional half-spaces.

**Fixed-function.** Implemented in dedicated hardware (the polymorph engine on NVIDIA).

**Guard band:** In practice, GPUs use a guard band — they don't clip to the exact viewport edges for X/Y; they use a larger region and let the rasterizer discard pixels outside the viewport. This avoids expensive new-vertex generation for triangles that are only slightly outside. Clipping is only strictly necessary for the near/far planes and for very large triangles that would overflow the rasterizer's coordinate precision.

### 1.7 Perspective Divide

```
NDC.x = clip.x / clip.w
NDC.y = clip.y / clip.w
NDC.z = clip.z / clip.w
```

This converts from 4D homogeneous clip space to 3D Normalized Device Coordinates. After this, the visible volume is a cube ([-1,1]³ in GL, [-1,1]² × [0,1] in Vulkan/D3D default).

### 1.8 Viewport Transform

Maps NDC to **window coordinates** (pixel positions + depth value):

```
window.x = viewport.x + (viewport.width / 2) * (NDC.x + 1)
window.y = viewport.y + (viewport.height / 2) * (NDC.y + 1)      // GL convention
window.z = (far - near) / 2 * NDC.z + (far + near) / 2           // GL depth range
```

Vulkan flips Y by default (Y increases downward) and uses depth [0,1].

### 1.9 Coordinate Spaces — The Full Chain

```
Object Space ──(Model Matrix)──▶ World Space ──(View Matrix)──▶ View/Eye Space
     │                                                               │
     │                                                        (Projection Matrix)
     │                                                               │
     ▼                                                               ▼
                                                              Clip Space (vec4)
                                                                     │
                                                           (Perspective Divide: /w)
                                                                     │
                                                                     ▼
                                                              NDC (vec3)
                                                                     │
                                                           (Viewport Transform)
                                                                     │
                                                                     ▼
                                                          Window/Screen Space
```

**Matrices between spaces:**

| Transform | Matrix | Notes |
|---|---|---|
| Object → World | Model (M) | Translation, rotation, scale |
| World → View | View (V) | Camera placement (inverse of camera transform) |
| View → Clip | Projection (P) | Perspective or orthographic. Encodes FOV, aspect, near/far |
| Clip → NDC | Perspective divide (/w) | Not a matrix — a per-component division |
| NDC → Window | Viewport transform | Defined by viewport + depth range parameters |

**Depth range conventions:**

| API | NDC Z range | Y-axis | Handedness (NDC) |
|---|---|---|---|
| OpenGL (default) | [-1, +1] | Up = +Y | Left-handed NDC |
| Vulkan | [0, 1] | Down = +Y (flipped) | Right-handed by convention |
| Direct3D | [0, 1] | Up = +Y | Left-handed |

The depth range matters enormously for precision — see reversed-Z in Section 7.

### 1.10 Backface Culling

Computes the signed area of the triangle in window coordinates. If the sign doesn't match the front-face winding order (CCW in GL by default), the triangle is discarded. This is a fixed-function test. The application sets the winding order and the cull mode (front, back, none).

### 1.11 Rasterization

**What it does:** For each primitive that survived culling, determines which **pixel samples** it covers. For each covered sample, generates a **fragment** with interpolated attributes (varyings from the vertex shader).

**Fixed-function.** This is one of the most important pieces of dedicated hardware on a GPU.

**Top-left fill rule:** When a triangle edge passes exactly through a sample point, the rasterizer uses a deterministic tie-breaking rule to ensure that adjacent triangles sharing an edge don't double-draw or leave gaps. The standard rule: a sample on a top edge or a left edge is considered inside. See Section 7 for details.

### 1.12 Early Depth/Stencil

Before the fragment shader runs, the hardware tests each fragment against the depth and stencil buffers. If the fragment is guaranteed to fail (it's behind an already-drawn surface), it is killed immediately, saving the cost of shading it.

**This is a critical optimization.** On modern GPUs, the majority of overdraw is eliminated by early-Z.

**What disables early-Z:** If the fragment shader writes to `gl_FragDepth` (modifying depth), uses `discard`/`clip()`, or if alpha-test (legacy) is enabled, then the hardware cannot know the final depth before the shader runs, so it must fall back to late depth testing. This is a **classic interview question**.

### 1.13 Fragment / Pixel Shader (FS / PS)

**What it does:** Runs once per fragment (or once per sample in some MSAA modes). Computes the output color(s). May also output a modified depth value. Can sample textures, compute lighting, perform arbitrary math.

**Programmable:** Yes — this is typically the most expensive shader.

**Data flow:**
- **In:** Interpolated varyings, fragment position, uniforms, textures, samplers
- **Out:** Color(s) for one or more render targets, optionally modified depth

### 1.14 Late Depth/Stencil

Fragments that survived the fragment shader are tested against depth/stencil now, if early-Z was disabled for this draw. Even if early-Z was active, the depth buffer is updated here with the final depth value.

### 1.15 Blending

**What it does:** Combines the fragment shader's output color with the existing color in the framebuffer. Configured via blend equation (add, subtract, min, max) and blend factors (src alpha, one minus src alpha, etc.).

**Fixed-function** — the application configures the blend state, but the computation is done in hardware.

**Order dependence:** Blending is **not commutative** for alpha blending. The result depends on the order fragments are processed. This is the "order-independent transparency" problem (OIT). For opaque rendering, order doesn't matter because the depth test resolves it.

### 1.16 ROP and Framebuffer Write

The **Raster Operations Pipeline** (ROP) is the final fixed-function unit. It performs:
- The depth/stencil write
- The blending
- The final color write to the framebuffer

ROPs operate on compressed color and depth data in memory and maintain on-chip caches.

### Interview Q&A — Section 1

**Q: Walk me through what happens from when the application issues a draw call to when a pixel appears in the framebuffer.**
A: The draw call tells the GPU to start the pipeline. Input Assembly reads vertices from the bound vertex buffer, possibly using an index buffer for deduplication, and groups them into primitives based on the topology — triangles, usually. Each vertex goes through the vertex shader, which transforms it from object space to clip space by multiplying by the model-view-projection matrix. If tessellation is active, patches get subdivided. The geometry shader can amplify or discard primitives. Clipping removes anything outside the view frustum, the perspective divide maps to NDC, and the viewport transform gives us pixel coordinates. Backface culling throws away triangles facing away. The rasterizer determines which pixels each triangle covers and generates fragments with interpolated attributes. Early depth/stencil kills occluded fragments before shading. The fragment shader computes color. Late depth/stencil handles cases where early-Z was disabled. Blending combines with the framebuffer, and the ROP writes the final result.
*Follow-up:* What coordinate space is `gl_Position` in, and why? — It's in clip space — a homogeneous 4-component vector. It must be in clip space because clipping must happen before the perspective divide. If you divided first, the clipping math would be non-linear and much more complex. The hardware clips against `±w` planes, which is a linear test in homogeneous coordinates.

**Q: What is the difference between the tessellation control shader and the tessellation evaluation shader?**
A: The control shader (hull shader in D3D) runs once per control point in a patch and decides the tessellation levels — how finely the tessellator should subdivide. It can also modify the control points themselves, for example doing LOD-based displacement. The tessellator is fixed-function hardware that takes those levels and generates a set of parametric coordinates within the domain. Then the evaluation shader (domain shader) runs once per generated vertex, receives the parametric coordinate and the patch's control points, and computes the actual position by evaluating the surface equation — like a Bézier surface evaluation. The key insight is that the TCS controls "how much" and the TES controls "where."
*Follow-up:* Why is tessellation better than just sending more triangles from the CPU? — Because the LOD can be computed per-patch on the GPU based on distance to camera. The CPU sends coarse patches; the GPU amplifies. This saves bus bandwidth and lets you do continuous LOD without CPU re-meshing.

**Q: Why is the geometry shader considered harmful for performance?**
A: The geometry shader can output a variable number of primitives, and the hardware has no way to know in advance how much output space to allocate. This means the GPU must be conservative with buffer allocation, and it serializes the work — the output of invocation N must be written before invocation N+1's output so that primitive ordering is preserved. On NVIDIA hardware, the GS also goes through the same SM pipeline as vertex shaders but with worse occupancy because of the output buffer requirements. Mesh shaders solve this by using cooperative thread groups with explicit output counts.
*Follow-up:* What replaced it? — Mesh shaders (and task shaders for amplification). They use a compute-like model with explicit output count declarations, which lets the hardware allocate output space efficiently.

**Q: Explain the guard band in clipping.**
A: The GPU doesn't clip triangles to the exact viewport edges in X and Y. Instead, it uses a much larger "guard band" region — often thousands of pixels beyond the viewport. Triangles that fall within the guard band are sent to the rasterizer without clipping, and the rasterizer simply doesn't generate fragments for pixels outside the viewport. This is much faster than generating new clip vertices. Only triangles that extend beyond the guard band — or that cross the near or far planes — actually get geometrically clipped. The guard band exists because clipping is expensive (it generates new vertices and attributes) and the rasterizer already has per-pixel coverage tests.
*Follow-up:* When must you actually clip, even with a guard band? — Near plane clipping is always required because vertices behind the camera produce inverted projections and the rasterizer can't handle negative `w`. Far plane clipping is also necessary. And if a triangle is so large it overflows the rasterizer's fixed-point coordinate precision, the guard band limit is hit and you must clip.

**Q: Why does OpenGL use a [-1,1] depth range while Vulkan and D3D use [0,1]?**
A: Historical reasons, mostly. OpenGL's [-1,1] range was symmetric and seemed mathematically clean, but it wastes half the floating-point precision — floating-point numbers have more precision near zero, and mapping the visible range to [-1,1] means the transition from negative to positive wastes bits on the sign. The [0,1] range gives better depth precision because you use the full mantissa range in one direction. This is also why reversed-Z became standard — by mapping the near plane to 1.0 and the far plane to 0.0 in a [0,1] range, you exploit the fact that floats have more precision near zero, which counteracts the 1/z nonlinearity of perspective projection. Vulkan defaulted to [0,1] partly because of this lesson.
*Follow-up:* Can you use [0,1] depth in OpenGL? — Yes, with `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` introduced in GL 4.5 / `ARB_clip_control`.

---

## 2. How the Logical Pipeline Maps to Real GPU Hardware

### 2.1 Unified Shader Architecture

Modern GPUs (since ~2006, NVIDIA G80) use a **unified shader architecture**: the same Streaming Multiprocessors (SMs) execute vertex shaders, fragment shaders, tessellation shaders, geometry shaders, and compute shaders. There is no dedicated "vertex processing unit" or "pixel processing unit."

**Why?** Load balancing. A scene with a million tiny triangles is vertex-heavy; a scene with a fullscreen quad and an expensive fragment shader is pixel-heavy. With dedicated units, one type would be idle. Unified architecture lets the GPU scheduler assign warps of any type to any SM based on current demand.

The workload manager distributes work in approximately this order:
1. Vertex/tessellation/geometry work → packaged into warps, dispatched to SMs
2. Completed primitives → sent to fixed-function raster engine
3. Generated fragments → packaged into warps, dispatched to SMs for fragment shading
4. Shaded fragments → sent to ROPs

### 2.2 Fragments, Quads, and Derivatives

This is critical and often asked about.

**2x2 Quad rule:** The rasterizer doesn't dispatch individual fragments. It dispatches **2x2 quads** — groups of four fragments corresponding to a 2x2 pixel block. Even if only one pixel in the quad is actually covered by the triangle, all four fragment shader invocations run.

```
  ┌───┬───┐
  │ A │ B │    If the triangle only covers pixel C,
  ├───┼───┤    all four (A, B, C, D) still execute.
  │ C │ D │    A, B, D are "helper lanes."
  └───┴───┘
```

**Why?** Because `dFdx()` and `dFdy()` (screen-space derivatives) are computed by subtracting values between adjacent lanes in the quad:

```
dFdx(value) = value[right] - value[left]     // B-A or D-C
dFdy(value) = value[bottom] - value[top]      // C-A or D-B
```

These derivatives are essential for **texture LOD selection** (mipmap level). The GPU compares how fast the texture coordinates change across the screen to decide which mip level to sample. Without the quad, you can't compute this.

**Helper lanes** execute the shader but their results are discarded — they don't write to the framebuffer. They exist solely to provide derivative values to the real fragments.

**Quad overshading:** At triangle edges, many quads will have helper lanes. For tiny triangles (common in dense geometry), most lanes in most quads may be helpers — this is a significant source of inefficiency. A triangle that covers exactly 1 pixel wastes 75% of the shader work. This is why "lots of tiny triangles" is a known performance problem.

### 2.3 How Pixels Map onto Warps

On NVIDIA hardware, a warp is 32 threads. For fragment shading:
- Each quad is 4 threads
- 8 quads = 32 threads = 1 warp
- A warp of fragments typically comes from the same triangle (or a small number of triangles in some implementations)

The raster engine groups quads into warps and dispatches them to SMs. This grouping affects coherence: fragments from the same triangle tend to access the same textures at similar coordinates, which is good for cache locality.

### 2.4 The Raster Engine

NVIDIA GPUs have dedicated raster engines (one per GPC — Graphics Processing Cluster). The raster engine:
1. Receives triangles in screen space
2. Determines which tiles/pixels they cover (edge function evaluation)
3. Generates fragment quads
4. Performs coarse and fine rasterization

**Tiled/Binned rasterization:** Mobile GPUs (ARM Mali, Qualcomm Adreno, Apple) use a two-pass approach: first bin all triangles into screen-space tiles, then render one tile at a time entirely on-chip. This saves memory bandwidth because the tile's color/depth stays in fast local memory. Vulkan's render pass / subpass system was designed to express this. Desktop NVIDIA GPUs do a form of tiling within GPCs but are fundamentally "immediate mode" renderers — they process triangles in submission order across the whole screen.

### 2.5 Hierarchical Z and Z-Cull

```
┌─────────────────────────────────────────────┐
│           Coarse Rasterization              │
│   (tile-level coverage, e.g., 8x8 pixels)  │
│                                             │
│         ┌──────────────────────┐            │
│         │  Hi-Z / Z-Cull Test  │            │
│         │  (per-tile Zmin/Zmax)│            │
│         └─────────┬────────────┘            │
│                   │ tiles that might pass    │
│                   ▼                          │
│           Fine Rasterization                │
│   (per-pixel/sample coverage)               │
│                                             │
│         ┌──────────────────────┐            │
│         │  Early-Z Test        │            │
│         │  (per-sample)        │            │
│         └─────────┬────────────┘            │
│                   │ surviving fragments      │
│                   ▼                          │
│           Fragment Shader                   │
└─────────────────────────────────────────────┘
```

**Hi-Z** maintains a coarse depth buffer — for each tile (e.g., 8x8 pixels), it stores the nearest and farthest Z values. Before fine rasterization, it checks: "Is this triangle's Z definitely behind the entire tile?" If so, the tile is culled without per-pixel work. This is enormously effective for front-to-back rendering.

**What disables early-Z (classic interview list):**
1. Fragment shader writes `gl_FragDepth` — depth is modified, can't test before shader
2. Fragment shader uses `discard` (or `clip()`) — coverage changes in shader
3. Legacy alpha test (`glAlphaFunc`) — coverage depends on shader output
4. The shader enables `layout(early_fragment_tests)` in GLSL — this FORCES early-Z but the programmer promises not to use discard (or accepts that discarded fragments still wrote depth)
5. Some stencil operations that depend on shader output
6. UAV/SSBO writes in the fragment shader on some implementations (side effects must be ordered correctly)

### 2.6 ROPs, Blending, and Compression

**ROPs (Raster Operations Units):** Fixed-function hardware that performs:
- Depth test + depth write (final)
- Stencil test + stencil write
- Blending (src × srcFactor OP dst × dstFactor)
- Color write (with channel mask)

ROPs are separate from SMs. Each ROP partition handles a region of the framebuffer. The number of ROPs limits fill rate (pixels/second for blending).

**Framebuffer and depth compression:** GPUs use lossless compression on color and depth surfaces. The compression works on tiles (e.g., 8x8) and exploits spatial coherence — a tile that's a single solid color can be stored as just that color plus a flag. This compression is transparent to shaders but dramatically reduces bandwidth. When a shader writes a complex pattern, the tile "falls out" of compression and uses full bandwidth. Compression state is tracked per-tile with metadata.

### 2.7 Texture Units, Samplers, and Filtering

Each SM has texture units (TMUs). A texture sample request goes through:

1. **Address calculation:** Compute (u,v) → texel coordinates, apply wrap mode (repeat, clamp, mirror)
2. **LOD selection:** Using screen-space derivatives (from quad), compute the mip level. Anisotropic filtering samples multiple mip levels along the direction of greatest change.
3. **Filtering:** Bilinear = weighted average of 4 texels. Trilinear = bilinear from two mip levels, then blend. Anisotropic = multiple bilinear/trilinear samples along the anisotropy axis.
4. **Cache:** Texels are fetched through a texture cache hierarchy (L1 per-SM, L2 shared). Spatial locality in the 2x2 quad helps cache hit rates enormously.

**Anisotropic filtering:** When a surface is viewed at a steep angle, the texture footprint in one axis is much longer than the other. Iso-tropic (same in all directions) filtering blurs. Anisotropic filtering takes multiple samples along the long axis, preserving detail. The hardware does this by evaluating the Jacobian of the texture coordinate mapping.

### 2.8 MSAA

**Multi-Sample Anti-Aliasing** decouples coverage from shading:
- The rasterizer evaluates coverage at N sample positions per pixel (e.g., 4x MSAA = 4 samples)
- The fragment shader runs **once per pixel** (not once per sample, usually)
- The shader's color is written to all covered samples
- Depth is tested/written per sample
- The resolve pass averages the samples to produce the final pixel color

This is much cheaper than supersampling (SSAA), where the shader runs per sample.

**Coverage vs shading rate:** MSAA proves that these are separable. Variable Rate Shading (VRS) takes this further — you can shade at coarser than per-pixel rates (one invocation per 2x2 or 4x4 block) while still rasterizing at full resolution. Useful for VR peripheral vision or areas with low visual complexity.

### 2.9 Load Balancing: Geometry-Heavy vs Pixel-Heavy

The unified architecture helps, but there are still bottlenecks:
- **Geometry-bound:** Many small triangles → vertex shading and setup dominate. Quads have many helper lanes. The raster engine can become the bottleneck.
- **Pixel/fill-bound:** Large triangles, expensive fragment shaders → SMs spend most time on fragment work. ROPs can bottleneck on blending.
- **Bandwidth-bound:** Large textures, high resolution, MSAA → memory bandwidth limits throughput.

For verification, you want tests that stress each bottleneck independently to isolate hardware bugs.

### Interview Q&A — Section 2

**Q: Why do GPUs shade fragments in 2x2 quads?**
A: Texture sampling requires screen-space derivatives to select the right mipmap level. The GPU computes `dFdx()` by subtracting the value of a varying between horizontally adjacent pixels, and `dFdy()` between vertically adjacent pixels. This requires at minimum a 2x2 group of fragment shader invocations running simultaneously. The "helper lane" fragments that aren't actually covered by the triangle still execute the shader to provide these derivative values, but their framebuffer writes are masked off. This is a fundamental architectural choice — without it, you'd need an alternative mechanism for LOD selection, and nothing faster has been found.
*Follow-up:* What's the worst-case efficiency of quad shading? — A single-pixel triangle uses a full quad: 4 threads execute, 3 are helpers. That's 25% efficiency. For a mesh of many tiny triangles, the average can be well below 50%. This is why the mesh shader pipeline includes primitive culling — to discard micro-triangles before they reach the rasterizer.

**Q: What is hierarchical-Z and why does it matter?**
A: Hi-Z maintains a coarse depth representation — typically one min/max depth value per tile of 8x8 or 16x16 pixels. When a new triangle arrives, the rasterizer first checks its depth against the tile's stored values. If the triangle is entirely behind the farthest sample in the tile, the entire tile is skipped without per-pixel depth tests. This eliminates massive amounts of overdraw, especially when rendering front-to-back. It's why sorting opaque objects front-to-back is a common optimization. The Hi-Z buffer is much smaller than the full Z-buffer so it stays in fast on-chip memory.
*Follow-up:* Does Hi-Z work with all rendering modes? — No. If early-Z is disabled (because the shader writes depth or uses discard), Hi-Z can't safely cull tiles because the shader might produce a passing depth value. Some implementations can still use Hi-Z conservatively in "may kill" mode but not "will kill" mode.

**Q: How does MSAA differ from supersampling?**
A: In MSAA, the rasterizer evaluates coverage at multiple sample points per pixel — say 4 for 4x MSAA — but the fragment shader runs only once per pixel, at the pixel center. The shader's color output is broadcast to all covered samples. Depth and stencil are evaluated per sample. During resolve, the per-sample colors are averaged to produce the final pixel. In contrast, supersampling runs the fragment shader at every sample position — 4x as expensive for shading. MSAA is much cheaper because shading cost doesn't scale with sample count, only the depth/stencil and bandwidth costs increase.
*Follow-up:* When does MSAA behave like supersampling? — When `centroid` or `sample` interpolation qualifiers are used, or when the shader uses `gl_SampleID`/`gl_SamplePosition`, the hardware promotes to per-sample shading for that draw call, effectively becoming supersampling.

**Q: Explain delta color compression for framebuffers.**
A: GPUs store a metadata table alongside the framebuffer. For each tile (e.g., 8x8 pixels), the metadata says whether the tile is in a compressed state. A tile where all pixels are the same color can be stored as just one color value — massive compression. A tile where colors differ only slightly can store a base color plus small deltas per pixel, using fewer bits. A tile with highly varying colors can't be compressed and is stored at full size. The GPU maintains this transparently: clears put tiles into compressed state, renders try to keep compression, and when a tile "blows out" the compression metadata is updated. This reduces memory bandwidth significantly without any quality loss.
*Follow-up:* How does this affect verification? — You need tests that exercise all compression modes: fully compressed (clears), partially compressed (gradients), and uncompressed (noise). Transitions between modes are also interesting because the hardware must correctly handle the metadata state machine.

**Q: What is variable rate shading?**
A: VRS allows the application to specify coarser shading rates for parts of the screen. Instead of one fragment shader invocation per pixel, you might shade one invocation per 2x2 block. The rasterizer still evaluates coverage at full resolution, so edges remain sharp, but the interior of surfaces gets fewer shader invocations. There are three tiers: per-draw (whole draw call at coarser rate), per-primitive (the vertex shader outputs a rate), and per-tile (a screen-space image specifies rates). The main use cases are VR (lower rate in periphery), motion blur (already blurry, don't need full rate), and performance scaling. It's conceptually the inverse of MSAA — where MSAA adds sample density for coverage, VRS reduces shading density for performance.
*Follow-up:* How does VRS interact with derivatives? — The quad size scales with the shading rate. At 2x2 coarse shading, the quad still exists but covers 4x4 pixels. Derivatives are coarser, which means mip selection may pick a more blurred level. This is usually acceptable for the use cases VRS targets.

---

## 3. The API Models, and Why They Differ

### 3.1 OpenGL: The State Machine

OpenGL is a **global state machine**. You set state, bind objects, and issue draw calls against the current state. Everything is implicit:

```cpp
// OpenGL: draw a textured triangle
glBindVertexArray(vao);
glUseProgram(shaderProgram);
glBindTexture(GL_TEXTURE_2D, texture);
glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, &mvp[0][0]);
glEnable(GL_DEPTH_TEST);
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
```

**Problems with this model:**

1. **Driver manages everything.** Memory allocation, synchronization, shader compilation, state validation — all hidden inside the driver. The driver is enormous and complex.
2. **Validation on every call.** The driver must check "is this combination of state valid?" at draw time. The state space is combinatorial — blend mode × depth mode × shader × vertex format × ... — so the driver often lazily recompiles shaders when it detects a new state combination.
3. **Implicit synchronization.** If you modify a buffer that's still in use by a previous draw call, the driver must either block (stall the CPU), or silently create a copy (renaming). The application has no visibility into this.
4. **Single-threaded by design.** The GL context is bound to one thread. Multi-threaded rendering requires context sharing, which is painful and driver-dependent.
5. **Unpredictable performance.** A seemingly innocent state change can trigger a shader recompile inside the driver, causing a multi-millisecond stall.

**Why this matters for tools:** If your stimulus framework mimics GL-style state, you inherit these problems. If it mimics Vulkan-style explicit state, you get predictability but more complexity.

### 3.2 Vulkan / D3D12: The Explicit Model

Vulkan and D3D12 push complexity to the application in exchange for predictability and performance.

#### Vulkan Object Relationships

```
VkInstance
 └── VkPhysicalDevice (one per GPU)
      └── VkDevice (logical device)
           ├── VkQueue (submit work here)
           │    └── belongs to a VkQueueFamily (graphics, compute, transfer, etc.)
           │
           ├── VkCommandPool (per queue family, per thread)
           │    └── VkCommandBuffer (record commands, submit to queue)
           │
           ├── VkDeviceMemory (explicit allocations from memory heaps)
           │    ├── VkBuffer (bound to a range of memory)
           │    └── VkImage (bound to a range of memory)
           │
           ├── VkPipeline (baked state: shaders + rasterization + blend + ...)
           │    └── created from VkPipelineLayout + VkRenderPass
           │
           ├── VkDescriptorSetLayout → VkDescriptorPool → VkDescriptorSet
           │    (describes how shaders access resources)
           │
           ├── VkRenderPass (describes attachments, subpasses, dependencies)
           │    └── VkFramebuffer (binds actual images to a render pass)
           │
           └── Sync primitives:
                ├── VkFence (CPU ↔ GPU: CPU waits for GPU)
                ├── VkSemaphore (GPU ↔ GPU: between queue submissions)
                ├── VkEvent (fine-grained: within or between command buffers)
                └── VkTimelineSemaphore (counter-based: flexible multi-point sync)
```

#### Key Design Choices

**Pipeline State Objects (PSOs):** All graphics state — shaders, vertex input, rasterization, depth/stencil, blending, multisampling — is baked into one immutable object at creation time. This means the driver can compile the final shader ISA once, upfront, with full knowledge of the state. No more draw-time recompiles.

```cpp
// Vulkan PSO creation (simplified)
VkGraphicsPipelineCreateInfo pipelineInfo{};
pipelineInfo.stageCount = 2;  // VS + FS
pipelineInfo.pStages = shaderStages;
pipelineInfo.pVertexInputState = &vertexInput;
pipelineInfo.pInputAssemblyState = &inputAssembly;
pipelineInfo.pRasterizationState = &rasterization;
pipelineInfo.pDepthStencilState = &depthStencil;
pipelineInfo.pColorBlendState = &colorBlend;
pipelineInfo.layout = pipelineLayout;
pipelineInfo.renderPass = renderPass;
vkCreateGraphicsPipelines(device, cache, 1, &pipelineInfo, nullptr, &pipeline);
```

**Descriptor Sets and Layouts:** Instead of binding resources one at a time (like `glBindTexture`), Vulkan groups resource bindings into descriptor sets. You define the layout upfront, allocate sets from a pool, write descriptors (bind actual buffer/image views), and bind whole sets at draw time. This is more efficient because the driver can translate a set into a hardware descriptor table once.

**Render Passes and Subpasses:** A render pass declares which images are used as attachments (color, depth, input) and how they transition between subpasses. This was motivated by **tile-based GPUs** (mobile): knowing the full set of subpasses upfront lets the driver keep everything in tile memory. On desktop GPUs, the driver may merge subpasses or treat them conventionally.

**Explicit Memory Management:** The application queries available memory types (device-local, host-visible, etc.), allocates `VkDeviceMemory`, and binds buffers/images to it. No more driver magic. You must handle aliasing, suballocation, and memory barriers yourself.

**Explicit Synchronization:**

```
Timeline:
  CMD 1 (vertex upload)   CMD 2 (draw)   CMD 3 (present)
  ──────────┬─────────────────┬────────────────┬──────
            │                 │                │
     Pipeline barrier    Semaphore         Fence
     (buffer transfer    (GPU→GPU:        (GPU→CPU:
      → vertex input)    render done →     frame done,
                          present)          CPU can
                                           reuse resources)
```

| Primitive | Scope | Use |
|---|---|---|
| `VkFence` | CPU ↔ GPU | CPU blocks until GPU finishes. Used to know when a frame's resources can be reused. |
| `VkSemaphore` | GPU ↔ GPU | Between queue submissions. Acquire swapchain image → render → present. |
| `VkTimelineSemaphore` | GPU ↔ GPU, GPU ↔ CPU | Counter-based. Wait/signal arbitrary values. More flexible than binary semaphores. |
| `VkEvent` | Within/between command buffers | Fine-grained. Set in one place, wait in another. Lighter than a full barrier. |
| Pipeline Barrier | Within a command buffer | Memory/execution dependency. E.g., "vertex buffer upload must complete before vertex fetch." |

**Image Layout Transitions:** Images have layouts (`VK_IMAGE_LAYOUT_UNDEFINED`, `COLOR_ATTACHMENT_OPTIMAL`, `SHADER_READ_ONLY_OPTIMAL`, `TRANSFER_DST_OPTIMAL`, `PRESENT_SRC_KHR`, etc.). The application must transition between them with pipeline barriers. This lets the hardware decompress/reformat the image data between uses. Getting transitions wrong causes corruption, not errors (unless validation layers are on).

### 3.3 Side-by-Side: Drawing a Triangle

| Aspect | OpenGL | Vulkan |
|---|---|---|
| Setup | ~50 lines | ~500-800 lines |
| State management | Set global state, draw | Bake into PSO, bind PSO, draw |
| Memory | Driver allocates | App allocates from heaps |
| Sync | Implicit (driver handles) | Explicit barriers, fences, semaphores |
| Multithreading | Painful (context per thread) | Natural (command buffer per thread) |
| Shader compilation | At link time + lazy recompile | At PSO creation (upfront, predictable) |
| Validation overhead | Every call | None at runtime (validation layers optional) |

**The tradeoff:** Vulkan trades more application code and more opportunities for bugs in exchange for predictable performance, thin drivers, and natural multithreaded command recording. For a stimulus framework, the explicit model is preferable because determinism and predictability matter more than development convenience.

### 3.4 D3D11 vs D3D12

D3D11 is similar to OpenGL in philosophy — implicit sync, driver-managed resources, state objects but still with significant driver intervention. D3D12 is Microsoft's explicit API, very similar to Vulkan in concept: command lists (= command buffers), root signatures (= pipeline layouts), descriptor heaps (= descriptor pools), pipeline state objects, and explicit barriers.

**Metal** (Apple): Explicit but slightly higher-level than Vulkan. No render passes with multiple subpasses (originally), simpler resource binding, and a unified memory model (CPU and GPU share memory on Apple Silicon).

**WebGPU:** A web-safe explicit API inspired by Vulkan/Metal/D3D12 but with guardrails (validation always on, no raw pointers). Useful reference, not critical for this interview.

### Interview Q&A — Section 3

**Q: Why did the industry move from OpenGL to Vulkan?**
A: OpenGL drivers became enormous because they had to manage everything implicitly — memory allocation, synchronization, shader recompilation when state changed, validation on every API call. This made performance unpredictable — a single state change could trigger a shader recompile inside the driver and stall for milliseconds. It also made multithreaded rendering nearly impossible because the API was designed around a single-threaded state machine bound to one context. Vulkan pushes all of that to the application: you explicitly manage memory, record command buffers on any thread, create pipeline state objects upfront so the driver compiles shaders once, and insert synchronization yourself. The result is a thinner driver, predictable performance, and natural parallelism.
*Follow-up:* What's the downside? — Much more code, more opportunities for bugs (missing barriers cause silent corruption), and a higher barrier to entry. Also, the application takes on responsibilities the driver used to handle — like suballocation and descriptor management — so the bugs shift from "driver bug" to "app bug."

**Q: Explain Vulkan pipeline state objects and why they exist.**
A: A PSO bakes all graphics state into one immutable object: shaders, vertex input format, primitive topology, rasterization mode, depth/stencil config, blend state, multisampling, render pass compatibility, and pipeline layout. This lets the driver compile the final shader ISA once at creation time, knowing exactly what state it'll execute with. In OpenGL, the driver often lazily recompiles shaders when it detects a state combination it hasn't seen before — like enabling a new blend mode with a particular shader. PSOs eliminate that entire class of stutter. The tradeoff is that you need one PSO per state combination, so you must plan ahead.
*Follow-up:* What if you need many PSO variants? — Use pipeline caches (`VkPipelineCache`) to save compiled results to disk, so subsequent runs load instantly. Also, pipeline libraries and graphics pipeline libraries (Vulkan 1.3+) allow composing PSOs from pre-compiled parts, reducing combinatorial explosion.

**Q: Walk me through the Vulkan synchronization primitives.**
A: There are four levels. Fences are CPU-GPU: you submit a command buffer with a fence and the CPU can wait on it, typically to know when a frame's resources are safe to reuse. Semaphores are GPU-GPU: you signal a semaphore when one queue finishes and wait on it in another submission — the classic use is acquire swapchain image, render, then signal a semaphore for the present queue. Timeline semaphores are a generalization: they have a monotonically increasing counter, and you can wait for or signal specific values, which makes multi-frame pipelining and multi-queue sync much cleaner. Events are fine-grained within command buffers — set an event in one place, wait in another. And pipeline barriers are the most common sync tool — they define a memory and execution dependency between pipeline stages within a command buffer.
*Follow-up:* What happens if you forget a barrier? — Undefined behavior. Typically, you'll see corrupted rendering — wrong data read because the cache wasn't flushed, or the image was in the wrong layout. Validation layers catch most of these, which is why you always develop with them on.

**Q: What are Vulkan render passes and why do they exist?**
A: A render pass describes the set of framebuffer attachments, how they're loaded at the start (clear, load, don't care), how they're stored at the end (store, don't care), and the subpasses that use them. This structure was designed for tile-based GPUs — mobile chips that render one tile at a time in on-chip memory. By declaring all subpasses upfront, the driver knows it can keep the tile in local memory across subpasses instead of flushing to main memory. On desktop GPUs, the driver often treats subpasses as independent render passes anyway, but the abstraction doesn't hurt. The load/store ops also help: `DONT_CARE` means the driver doesn't need to fetch the old contents from memory, saving bandwidth.
*Follow-up:* What's the "dynamic rendering" extension? — `VK_KHR_dynamic_rendering` (core in Vulkan 1.3) lets you begin rendering without pre-creating render pass and framebuffer objects. You specify attachments inline. This simplifies the API and matches what desktop GPUs do anyway. Render passes remain useful for mobile optimization.

**Q: How does Vulkan's memory model differ from OpenGL's?**
A: In OpenGL, you call `glBufferData` and the driver decides where in GPU memory to put it, whether to make a copy, and how to handle the case where you modify it while the GPU is reading it. In Vulkan, you query the physical device's memory types — some are device-local (fast VRAM, not CPU-visible), some are host-visible (CPU can map them, often slower for GPU access), some are both (on integrated GPUs or with resizable BAR). You allocate `VkDeviceMemory` from a specific heap, then bind your buffer or image to a region of that allocation. You're responsible for suballocation, alignment, and not writing to memory the GPU is reading — there's no implicit renaming. This gives you full control over memory placement and eliminates hidden copies, but you must manage it correctly.
*Follow-up:* What's a staging buffer? — A host-visible buffer you write to from the CPU, then copy to a device-local buffer with a transfer command. This two-step pattern exists because device-local memory is fast for the GPU but often not CPU-accessible on discrete GPUs.

---

## 4. Shaders and Shader Compilation

### 4.1 The Compilation Pipeline

```
  GLSL / HLSL  (human-authored source)
       │
       ▼
  SPIR-V / DXIL  (portable intermediate representation)
       │
       ▼
  Vendor IR  (NVIDIA PTX → SASS, AMD IL → ISA)
       │
       ▼
  Hardware ISA  (actual instructions executed by SMs/CUs)
```

**Offline compiler** (glslc, dxc, glslangValidator): Parses the high-level language, performs front-end optimizations, and emits SPIR-V (for Vulkan/OpenGL) or DXIL (for D3D12). This is deterministic and can be done at build time.

**Driver compiler:** Takes SPIR-V/DXIL plus pipeline state (blend mode, vertex format, etc.) and compiles to the GPU's native ISA. This is where vendor-specific optimizations happen: register allocation, instruction scheduling, occupancy tuning. This step happens at `vkCreateGraphicsPipelines` time (Vulkan) or lazily at draw time (OpenGL).

**Specialization constants:** Vulkan lets you define constants in SPIR-V that are set at pipeline creation time. The driver can then optimize: a specialization constant of `false` for a branch lets the compiler dead-code-eliminate that path. This replaces the OpenGL pattern of `#define` variants with many shader permutations.

**Pipeline caches:** `VkPipelineCache` serializes compiled pipelines to disk. On subsequent runs, the driver can skip compilation for unchanged (shader + state) combinations. This is why the first run of a Vulkan app is slower — the cache is cold. **Shader compilation stutter** in games (visible as frame hitches) happens when a new PSO is needed mid-frame and the cache misses.

### 4.2 Resource Binding

**How shader resources become memory addresses on hardware:**

```
Application-side:                    Hardware-side:
┌──────────────────┐                ┌──────────────────────────┐
│ Descriptor Set 0 │ ──compiled──▶  │ Hardware descriptor table │
│  binding 0: UBO  │                │  slot 0: base addr + size│
│  binding 1: tex  │                │  slot 1: tex handle      │
│  binding 2: SSBO │                │  slot 2: base addr + size│
└──────────────────┘                └──────────────────────────┘
```

| Concept | Vulkan | D3D12 | OpenGL |
|---|---|---|---|
| Resource group | Descriptor Set | Descriptor Table (in Root Signature) | N/A (bind individually) |
| Layout declaration | VkDescriptorSetLayout | Root Signature | Uniform locations |
| Fast small constants | Push Constants (128-256 bytes) | Root Constants (64 DWORDs) | glUniform* |
| General buffer | Storage Buffer (SSBO) | UAV / SRV | SSBO / UBO |
| Bindless | Descriptor indexing extension | Unbounded descriptor arrays | ARB_bindless_texture |

**Push constants / Root constants:** Small amounts of data (like an MVP matrix or a material index) passed inline with the command buffer, avoiding a descriptor set update. They map to hardware registers or a small fast buffer.

**Bindless:** Instead of binding specific textures to specific slots, you put all textures in a large descriptor array and index into it with an integer in the shader. This removes binding changes between draws — enormous performance win for scene rendering with many materials.

### 4.3 Interpolation and Varyings

Vertex shader outputs are **interpolated** across the triangle by the rasterizer before being delivered to the fragment shader. Interpolation modes:

- `smooth` (default): Perspective-correct interpolation. Divides by w at vertices, interpolates, then multiplies by w at the fragment. This is essential for texture coordinates.
- `flat`: No interpolation — the value from the provoking vertex is used for the entire primitive.
- `noperspective`: Linear interpolation in screen space (no perspective correction). Useful for screen-space effects.
- `centroid`: Evaluated at a sample position guaranteed to be inside the primitive. Prevents artifacts at triangle edges with MSAA.
- `sample`: Evaluated per sample — triggers per-sample shading.

**Built-in variables (GLSL):**

| Variable | Stage | Meaning |
|---|---|---|
| `gl_Position` | VS out | Clip-space position (vec4) |
| `gl_FragCoord` | FS in | Window-space position (x, y, z=depth, w=1/w) |
| `gl_FrontFacing` | FS in | Is this fragment front-facing? |
| `gl_FragDepth` | FS out | Override depth (disables early-Z!) |
| `gl_VertexID` | VS in | Index of current vertex |
| `gl_InstanceID` | VS in | Index of current instance |
| `gl_SampleID` | FS in | Current sample index (triggers per-sample shading) |
| `gl_PrimitiveID` | GS/FS in | Index of current primitive |

### 4.4 Uniform/Constant Buffers vs Storage Buffers

| Feature | UBO / Constant Buffer | SSBO / UAV |
|---|---|---|
| Size | Limited (64KB typical for UBO) | Large (up to buffer max) |
| Access | Read-only in shader | Read-write |
| Performance | Cached aggressively, may broadcast | Random access, less caching |
| Use case | MVP matrices, material params | Particle buffers, indirect args, general data |

### Interview Q&A — Section 4

**Q: What is SPIR-V and why does it exist?**
A: SPIR-V is a portable binary intermediate representation for shaders, standardized by Khronos. It exists because different GPU vendors need different final ISAs, but the industry wanted a common interchange format that's more structured than source code. Compared to shipping GLSL source, SPIR-V eliminates the need for a full parser and front-end in the driver, reduces driver complexity, and makes compilation more predictable. It also enables language diversity — you can compile GLSL, HLSL, or even domain-specific languages to SPIR-V. The driver's compiler takes SPIR-V plus pipeline state and produces vendor ISA.
*Follow-up:* What's the equivalent in D3D12? — DXIL (DirectX Intermediate Language), which is based on LLVM IR. Same concept: a portable bytecode the driver backend compiles to hardware ISA.

**Q: Why does shader compilation stutter exist, and how is it mitigated?**
A: Shader stutter happens when a new pipeline state combination is needed for the first time during rendering. The driver must compile SPIR-V to ISA, which can take milliseconds. In OpenGL, this was even worse because the driver didn't know the full state until draw time and might lazily recompile. Mitigation: pipeline caches (`VkPipelineCache`) save compiled results to disk so subsequent runs are instant. Pre-warming the cache during loading screens by creating all expected PSOs upfront. Pipeline libraries let you precompile parts independently. Async compilation on a background thread (application presents the previous frame while compiling). Some engines use "uber-shaders" — a single huge shader with branches, compiled once, and then specialize later.
*Follow-up:* Does this affect a GPU verification framework? — Absolutely. If the framework triggers shader compilation mid-test, the timing becomes unpredictable and you can't get deterministic cycle counts. You want all compilation done during setup, which is another argument for the Vulkan PSO model.

**Q: Explain push constants.**
A: Push constants are a small block of data (typically 128-256 bytes, the minimum guaranteed by Vulkan is 128) that you embed directly into the command buffer rather than going through a descriptor set. The driver maps them to hardware registers or a fast constant buffer. They're ideal for per-draw data that changes frequently — like a model matrix or a material ID — because updating a push constant is essentially free compared to updating a descriptor set. The tradeoff is the small size limit.
*Follow-up:* What's the hardware mechanism? — On NVIDIA, push constants map to a "constant buffer window" — a small region of constant memory that the SM can access in a single cycle. Larger constant buffers go through the L1 constant cache, which is still fast but has a lookup cost.

**Q: What does `centroid` interpolation do and when do you need it?**
A: With MSAA, some sample positions may be outside the triangle even though the pixel's center is inside (or vice versa). If a varying is interpolated at the pixel center, which is outside the triangle, it may extrapolate beyond the vertex values — causing artifacts like colors outside the valid range. Centroid interpolation evaluates the varying at a sample position guaranteed to be inside the primitive. This prevents the extrapolation but makes the interpolation location variable, which means derivatives of centroid-interpolated values are unreliable.
*Follow-up:* When would you NOT want centroid? — When you need reliable derivatives for mip selection. Centroid positions can be different between adjacent pixels, making `dFdx`/`dFdy` noisy. Typically, you use centroid for color/opacity but not for texture coordinates.

**Q: What is bindless rendering?**
A: Bindless rendering replaces the traditional model of binding specific textures to specific slots before each draw call. Instead, you make all textures "resident" and store their handles (64-bit GPU addresses or descriptor indices) in a buffer. The shader indexes into this buffer with an integer — often a material ID stored per vertex or per instance. This eliminates the need to change texture bindings between draws, allows arbitrary numbers of textures per draw, and dramatically reduces CPU overhead. On NVIDIA, `ARB_bindless_texture` provides GPU addresses directly. On Vulkan, descriptor indexing with large descriptor arrays achieves the same thing.
*Follow-up:* How does the hardware resolve a bindless texture access? — The shader loads the descriptor index from memory, uses it to index into a large descriptor heap in GPU memory, and the texture unit fetches the descriptor (which contains the base address, format, dimensions, mip count) to set up the texture fetch. It's essentially an extra indirection compared to a pre-bound texture, but the cost is amortized by the cache.

---

## 5. Compute Shaders and Interop

### 5.1 Compute Shaders: Same Hardware, Different Front End

Compute shaders run on the same SMs that run vertex and fragment shaders. The execution model:

| Concept | Vulkan/GL Compute | CUDA |
|---|---|---|
| Work unit | Invocation | Thread |
| Group | Workgroup | Thread block |
| Grid | Dispatch | Grid |
| Shared memory | `shared` (GLSL) / `groupshared` (HLSL) | `__shared__` |
| Barrier | `barrier()` | `__syncthreads()` |
| Group size | `layout(local_size_x=256)` | `<<<grid, block>>>` |
| Dispatch | `vkCmdDispatch(gx, gy, gz)` | `kernel<<<grid, block>>>()` |

**Key differences:**
- Compute shaders in graphics APIs go through the same pipeline state / descriptor binding model. CUDA uses a different host API with simpler parameter passing.
- CUDA exposes more hardware: warp-level primitives (`__shfl_sync`, `__ballot_sync`), cooperative groups, tensor cores. Graphics compute shaders are catching up (subgroup operations in Vulkan).
- Same ISA at the end — the SM doesn't care which API launched the warp.

### 5.2 When Graphics Teams Use Compute

- **GPU-driven culling:** A compute shader reads bounding boxes, does frustum/occlusion culling, and writes indirect draw arguments. Only visible objects get drawn.
- **Post-processing:** Bloom, tone mapping, depth of field, motion blur — these are full-screen image operations that map naturally to compute.
- **Particle simulation:** Update particle positions/velocities in a compute shader, then render them with a graphics pipeline.
- **Hi-Z pyramid generation:** Build a mipmap of the depth buffer for occlusion culling.

### 5.3 CUDA/Graphics Interop

When you need CUDA and graphics to share data (e.g., a CUDA simulation feeding a graphics renderer), there are two mechanisms:

**Legacy (GL-CUDA interop):**
```cpp
// Register GL buffer with CUDA
cudaGraphicsGLRegisterBuffer(&resource, glBuffer, cudaGraphicsRegisterFlagsNone);
// Map for CUDA access
cudaGraphicsMapResources(1, &resource, stream);
cudaGraphicsResourceGetMappedPointer(&devPtr, &size, resource);
// CUDA kernel uses devPtr
myKernel<<<grid, block>>>(devPtr);
// Unmap
cudaGraphicsUnmapResources(1, &resource, stream);
```

**Modern (External memory / semaphores — Vulkan extensions):**
- `VK_KHR_external_memory` — export a Vulkan buffer/image as a POSIX fd or Win32 handle; CUDA imports it via `cudaImportExternalMemory`. Zero-copy sharing.
- `VK_KHR_external_semaphore` — share sync primitives between Vulkan and CUDA. Signals in one API are waited on in the other.

This is a direct hardware sharing mechanism — the same physical memory is accessed by both pipelines, with explicit synchronization.

### 5.4 Why a GPU Tools Engineer Must Understand Both

The SMs are shared. A verification test might:
1. Launch a compute shader to fill a buffer with test data
2. Use that buffer as vertex data in a graphics pipeline
3. Read back the framebuffer and compare to expected output

If the test fails, you need to know whether the bug is in compute dispatch, in the graphics pipeline, or in the synchronization between them. Understanding both paths — and knowing they compile to the same ISA and run on the same hardware — is essential.

### Interview Q&A — Section 5

**Q: How do Vulkan compute shaders differ from CUDA kernels at the hardware level?**
A: At the hardware level, they're nearly identical. Both compile down to the same ISA (SASS on NVIDIA), both run on the same SMs, both use the same register file, shared memory, and cache hierarchy. The differences are in the host-side API — CUDA uses its own runtime with `cudaMalloc`, `cudaMemcpy`, and `<<<>>>` launch syntax, while Vulkan compute shaders go through descriptor sets, command buffers, and pipeline barriers. CUDA also exposes more hardware features like warp shuffles, cooperative groups, and tensor core instructions, though Vulkan has been adding subgroup operations that expose similar functionality. The driver's compiler backend is largely shared.
*Follow-up:* Can you mix them in the same application? — Yes, via interop. You can share memory between Vulkan and CUDA using external memory extensions, and synchronize using external semaphores. The GPU memory is physically shared, so it's zero-copy.

**Q: Why would a graphics team use compute shaders instead of the graphics pipeline?**
A: Compute shaders have a simpler execution model — no rasterization, no fixed-function stages, just a grid of threads. This makes them ideal for operations that don't map to the raster pipeline: arbitrary read-write to buffers and images, reductions, prefix sums, sorting, simulation. Specific examples: GPU-driven culling writes indirect draw arguments, post-processing effects like bloom are full-screen image operations, and particle systems need scattered read-write. Compute also avoids the overhead of setting up a render pass and rasterization state for something that's fundamentally just a parallel computation over data.
*Follow-up:* Any downsides to compute vs fragment shaders for full-screen passes? — Fragment shaders get free quad-based derivatives and can use the ROP blend hardware. A compute shader doing the same work must implement blending manually (read-modify-write with atomics or just overwrite). Also, the rasterizer's quad dispatch gives better cache locality for texture fetches in some patterns. But compute is more flexible and often wins for complex multi-pass operations.

**Q: Explain external memory and external semaphores in Vulkan.**
A: External memory extensions let you export a Vulkan buffer or image's underlying memory as an OS-level handle — a file descriptor on Linux or a Win32 handle on Windows. Another API, like CUDA or another Vulkan instance, can import that handle and access the same physical memory. External semaphores do the same for synchronization: one API signals, the other waits. Together, they enable zero-copy interop between graphics and compute APIs. The key constraint is that only one API should be actively accessing the memory at a time — you must synchronize properly.
*Follow-up:* How does this differ from the legacy `cudaGraphicsGLRegisterBuffer` approach? — The legacy approach uses driver-internal mechanisms specific to GL-CUDA on NVIDIA. External memory is a standard, vendor-agnostic mechanism that works across APIs and (in theory) across vendors. It's also more explicit about ownership and synchronization, fitting the Vulkan philosophy.

**Q: If a verification test uses both compute and graphics, what synchronization is needed between them?**
A: You need a pipeline barrier between the compute dispatch and the graphics draw. Specifically, a memory barrier that ensures the compute shader's writes to the buffer are visible to the vertex shader's reads. In Vulkan terms, you'd insert a `vkCmdPipelineBarrier` with source stage `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, destination stage `VK_PIPELINE_STAGE_VERTEX_INPUT_BIT`, and a buffer memory barrier with source access `VK_ACCESS_SHADER_WRITE_BIT` and destination access `VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT`. Missing this barrier is a common source of flaky test failures — the GPU might read stale data from cache.
*Follow-up:* Would this barrier look different on D3D12? — Yes, D3D12 uses resource state transitions (`D3D12_RESOURCE_STATE_UNORDERED_ACCESS` → `D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER`) and `ResourceBarrier` calls, but the concept is the same: make compute writes visible to graphics reads.

**Q: A test renders correctly on one GPU but produces a slightly different image on another. Both are from the same vendor but different architectures. Why?**
A: Floating-point arithmetic is not guaranteed to produce bit-identical results across architectures. Different SMs may have different FMA (fused multiply-add) behavior, different rounding modes for transcendentals, or different instruction scheduling that changes the order of operations. The rasterizer's coverage determination may use different precision fixed-point formats. Texture filtering hardware may round differently. Compiler optimizations may reassociate floating-point operations. The API specs intentionally allow implementation-defined variation in these areas — the conformance tests only require results within a tolerance, not bit-exact. For verification, you need per-architecture golden images or tolerance-based comparison.
*Follow-up:* How do you handle this in a test framework? — Define per-architecture golden images or use statistical comparison (PSNR, SSIM) with architecture-specific thresholds. For functional correctness, focus on invariants the spec does guarantee — like the rasterization rules — rather than exact pixel values.

---

## 6. Graphics as Verification Stimulus

### 6.1 What Makes a Good Graphics Test

A good graphics hardware verification test:
1. **Isolates a specific hardware feature** — test one thing at a time (just blending, just tessellation, just Hi-Z)
2. **Exercises corner cases** — degenerate inputs, boundary conditions, maximum/minimum values
3. **Has a deterministic expected output** — either a golden image or a per-pixel checksum
4. **Runs quickly** — verification cycles are expensive; each test should be minimal
5. **Is reproducible** — same input → same output, every time, on the same hardware

### 6.2 Determinism in Rendering

This is harder than it sounds:

- **Floating-point invariance:** The GLSL/SPIR-V `invariant` qualifier (and `precise` in HLSL) constrains the compiler to produce the same result for the same expression regardless of context. Without it, the compiler may reorder operations for performance, changing results due to FP non-associativity.
- **What APIs guarantee:** GL and Vulkan guarantee that the same draw call with the same state on the same implementation produces the same result (with `invariant`). They do NOT guarantee results match across vendors, or even across driver versions in some cases.
- **Rasterization rules:** The spec defines the diamond exit rule (for lines) and the top-left fill rule (for triangles) — these are deterministic. But the spec allows some latitude in exact sample positions and coverage determination.
- **Driver/compiler version sensitivity:** A driver update may recompile shaders differently — different instruction order, different register allocation — producing slightly different floating-point results. This is expected behavior, not a bug.

### 6.3 Golden Image Comparison

The most common verification approach:
1. Render a test scene
2. Read back the framebuffer
3. Compare pixel-by-pixel against a reference ("golden") image
4. Report mismatches

**Tolerance policies:**
- **Exact match:** Only for fully deterministic operations (integer arithmetic, specific clear colors)
- **Per-component threshold:** Allow ±1 LSB difference (common for floating-point rounding)
- **Per-pixel RMSE:** Allow small statistical error
- **Structural similarity (SSIM):** Measures perceptual similarity, more robust to minor shifts
- **CRC/checksum:** Hardware-computed CRC of the framebuffer — faster than readback + comparison

**Per-pixel CRC:** Some GPU verification environments compute a CRC of the framebuffer on the GPU itself, avoiding the slow readback path. This is common in silicon validation where readback bandwidth is limited.

### 6.4 Testing Fixed-Function Corner Cases

These are the bugs you're hunting:

| Corner Case | What to Test |
|---|---|
| Degenerate triangles | Zero-area triangles (collinear vertices), infinitely thin slivers |
| Clipping planes | Triangle partially behind near plane, triangle intersecting user clip plane |
| Guard band overflow | Very large triangles that exceed guard band limits |
| Depth precision | Z-fighting at extreme distances, reversed-Z correctness |
| Blend modes | All blend equations × all blend factors, especially the edge cases like `GL_ONE_MINUS_SRC_ALPHA` with alpha=1.0 |
| MSAA resolve | Different sample counts, centroid interpolation at triangle edges |
| Texture edge cases | Mip level transitions, anisotropic filtering at extreme angles, non-power-of-2 textures, border color, cubemap seams |
| Viewport edge | Triangles partially outside viewport, scissor/viewport interaction |
| Integer overflow | Very large vertex counts, maximum texture dimensions, maximum instance count |

### 6.5 Conformance Test Suites

- **Khronos CTS (Conformance Test Suite):** Thousands of tests for GL, Vulkan, OpenCL. Required to pass for a vendor to claim conformance. Open source. Good reference for what the spec requires.
- **WHQL (Windows Hardware Quality Labs):** Microsoft's certification. Includes D3D conformance tests. Required for "Windows certified" driver.
- **dEQP (drawElements Quality Program):** Now part of Khronos CTS. Widely used for mobile GPU validation.

These are *functional* tests — they verify API contract compliance. NVIDIA's internal stimulus framework likely goes deeper: stressing specific hardware blocks, hitting timing corner cases, and testing at the microarchitectural level.

### 6.6 Graphics vs Compute Stimulus

| Aspect | Graphics Stimulus | Compute Stimulus |
|---|---|---|
| State complexity | Very high (pipeline state, framebuffer, render pass) | Lower (kernel + buffers) |
| Fixed-function coverage | Critical (rasterizer, ROPs, tessellator, clipper) | Minimal |
| Output verification | Image comparison (visual or CRC) | Buffer comparison (exact or tolerance) |
| Determinism challenges | Higher (FP interpolation, rasterization rules) | Lower (but still FP issues) |
| Interaction with memory system | Through texture units, framebuffer compression | Through load/store, caches |
| State space to explore | Enormous (combinatorial explosion of state) | Large but more structured |

A good GPU tools engineer understands that the same physical hardware is being tested through different software paths, and can design tests that exercise both paths — and the transitions between them.

### Interview Q&A — Section 6

**Q: How would you design a test to verify that early-Z culling works correctly?**
A: I'd set up a simple scene: render a near quad first (establishing the depth buffer), then render a far quad behind it. With early-Z working, the far quad's fragments should be killed before the fragment shader runs. To verify this, I'd put a side effect in the far quad's fragment shader — like an atomic increment of a counter in an SSBO. If early-Z is working, the counter should be zero (or very small — some fragments at edges might not be culled by Hi-Z). Then I'd add a test variant where I enable `gl_FragDepth` writes in the far quad's shader, which should disable early-Z, and the counter should now equal the number of covered fragments. Comparing the two confirms early-Z behavior.
*Follow-up:* What about Hi-Z specifically? — For Hi-Z testing, use tiles of geometry. Render a near surface covering the whole screen, then render many small far triangles spread across tiles. Hi-Z should reject entire tiles without per-pixel tests. You can measure this by comparing shader invocation counts (via hardware performance counters if available) between the two-layer case and a single-layer case.

**Q: Why is golden image comparison hard for cross-driver or cross-architecture testing?**
A: Because the GPU specs intentionally allow implementation-defined behavior in many areas. Floating-point rounding of texture filtering, the exact sub-pixel sample positions for MSAA, the order of rasterization within a tile, the compiler's choice of FMA vs separate multiply-add — all of these can produce pixel-level differences that are spec-compliant on both sides. Even the `invariant` qualifier only guarantees consistency within a single compilation, not across different compilers. So golden images are typically per-platform: each architecture gets its own reference. Cross-architecture tests use tolerance-based comparison or test only strictly deterministic properties (like "did the clear color come through correctly" or "is the depth test rejecting the right fragments").
*Follow-up:* How would you handle a driver update that changes golden images? — Regenerate goldens with the new driver, but diff the old and new results first. If differences are within expected FP tolerance, accept the new goldens. If differences are large or structural, investigate whether the driver introduced a regression. This is a workflow problem — you need versioned golden images and a review process.

**Q: What makes graphics state space explosion a testing challenge?**
A: Consider just the blend state: 5 blend equations × ~15 src factors × ~15 dst factors × 2 (color and alpha separate) × on/off. That's thousands of combinations for blending alone. Multiply by depth/stencil states, vertex formats, primitive topologies, MSAA modes, texture formats, and shader variants — the state space is astronomical. You can't test every combination. Instead, you use combinatorial testing (pairwise/n-wise covering arrays), targeted tests for known risky interactions, and fuzz testing to explore the space randomly. Priority goes to states that exercise different hardware paths — like blending enabled vs disabled, which activates different ROP logic.
*Follow-up:* How do conformance test suites handle this? — They test a representative subset with known corner cases. The Khronos CTS has thousands of tests but still can't cover everything. That's why vendors have internal test suites that go deeper — NVIDIA's stimulus framework likely generates tests programmatically to get better coverage.

**Q: What's the role of CRC checking in hardware verification?**
A: CRC checking computes a hash of the framebuffer contents on the GPU itself, avoiding the need to read back the entire image to the CPU. This is much faster and can be done at wire speed in the display pipeline. In verification, the expected CRC is computed from the golden image offline, and the test just compares a single 32-bit or 64-bit value. This is especially valuable in silicon validation and emulation where readback bandwidth is extremely limited. The tradeoff is that a CRC tells you "match or no match" but doesn't tell you which pixels differ — so a failed CRC test needs a follow-up readback for diagnosis.
*Follow-up:* Can CRC checking give false passes? — Theoretically, two different images could produce the same CRC (hash collision), but with a good CRC polynomial this is astronomically unlikely. More practically, the risk is that the CRC itself is computed incorrectly by the hardware — which would be its own bug worth catching.

**Q: How does a graphics stimulus framework differ from just running a game or benchmark?**
A: A game exercises popular code paths with realistic workloads but doesn't systematically test corner cases, doesn't control state precisely, and doesn't have deterministic expected output. A stimulus framework is designed for verification: it constructs specific state combinations, issues specific draw calls with known inputs, and compares output against known-correct results. It's minimal — one test per feature, not a complex scene. It can also do things no game would: deliberately create degenerate geometry, use extreme depth ranges, exercise every blend mode, or trigger hardware-internal state transitions that are rare in practice but must work correctly. Games find bugs by accident; a stimulus framework finds bugs by design.
*Follow-up:* Would the framework be more like OpenGL or Vulkan? — Likely a custom abstraction that can target either. For test precision, Vulkan's explicit model is better because you control every state transition. But you might still need GL-style tests to validate the GL driver path. The framework probably has its own abstraction that maps to both.

---

## 7. Fundamentals You Must Not Fumble

### 7.1 Top-Left Fill Rule

When rasterizing, if a triangle edge passes exactly through a sample point, a deterministic rule prevents double-drawing or gaps between adjacent triangles.

**The rule:** A pixel is considered inside the triangle if its sample point lies on a "top" edge or a "left" edge. A top edge is exactly horizontal and above the other edges. A left edge is not horizontal and is on the left side of the triangle. This ensures shared edges are assigned to exactly one triangle.

This is a spec requirement in both OpenGL and D3D. Getting this wrong in hardware causes visible seams.

### 7.2 Depth Buffer Precision and Reversed-Z

Standard perspective projection maps near to 0 (or -1) and far to 1. But 1/z is highly nonlinear — most of the floating-point precision is wasted on objects close to the camera, and distant objects get almost no precision, causing z-fighting.

**Reversed-Z:** Map near to 1.0, far to 0.0. This exploits the fact that floating-point has more precision near zero — the distant objects (mapped to values near 0) now get the float precision they need. Combined with a [0,1] depth range and a floating-point depth buffer, reversed-Z dramatically reduces z-fighting.

```
Standard Z:   near=0.1 → 0.0,  far=1000 → 1.0
              Objects at distance 500 map to ~0.9998  (almost no precision left)

Reversed-Z:   near=0.1 → 1.0,  far=1000 → 0.0
              Objects at distance 500 map to ~0.0002  (excellent float precision)
```

### 7.3 Z-Fighting

When two surfaces have nearly the same depth, floating-point precision is insufficient to distinguish them, and the depth test alternates between passing and failing across pixels. This produces a shimmering, striped artifact.

**Fixes:** Reversed-Z, larger near plane distance, depth bias (`glPolygonOffset`), or avoid coplanar geometry.

### 7.4 Alpha Blending and Order Dependence

Standard alpha blending: `result = src.rgb × src.a + dst.rgb × (1 - src.a)`

This is **not commutative** — drawing A then B gives a different result than B then A. For correct transparency, you must sort back-to-front, which is expensive. This is the "order-independent transparency" (OIT) problem. Solutions include depth peeling, weighted blended OIT, and per-pixel linked lists — all approximate or expensive.

For opaque geometry, depth testing resolves order — draw order doesn't matter.

### 7.5 Gamma, sRGB, and Color Spaces

Human vision is more sensitive to dark variations than light ones. sRGB encoding compresses the linear color range so that perceptually uniform steps map to uniform code values in 8-bit storage.

**The pipeline:**
1. Textures are often stored in sRGB
2. The texture unit converts sRGB → linear on read (`GL_SRGB8_ALPHA8`)
3. All lighting and blending happens in **linear** space
4. The framebuffer converts linear → sRGB on write (if sRGB framebuffer is enabled)

Getting this wrong produces washed-out or overly dark images. In verification, you must know whether your reference images are linear or sRGB.

### 7.6 Texture Filtering Artifacts

- **Magnification:** Texture is too small for the screen area. Nearest filtering → blocky. Bilinear → smooth but blurry.
- **Minification:** Texture is too large for the screen area. Without mipmaps → aliasing (shimmering). With mipmaps → smooth but may be too blurry. Trilinear eliminates visible mip boundaries.
- **Anisotropic:** Prevents excessive blur at oblique angles.
- **Mip bias:** Shifts the LOD calculation to use a sharper or blurrier mip level.

### 7.7 Modern Additions (Concept Level)

**Ray Tracing Pipeline:**
- **BVH (Bounding Volume Hierarchy):** Acceleration structure built over scene geometry. Hardware RT cores traverse this.
- **RT cores:** Dedicated hardware for ray-BVH intersection tests. Returns intersection point to shader.
- **Shader Binding Table (SBT):** Maps geometry instance + ray type to the correct shader to run on hit/miss. Think of it as a dispatch table for ray interactions.
- **Pipeline stages:** Ray generation → intersection → any-hit → closest-hit / miss. These are programmable shader stages.

**Mesh Shaders:**
- Replace input assembly + vertex shader + tessellation + geometry shader with two stages:
  - **Task shader (amplification shader):** Decides how many mesh shader groups to spawn. Used for LOD, culling.
  - **Mesh shader:** Outputs vertices and primitives directly. Cooperative (compute-like) model with shared memory. Explicit output count.
- More efficient for dense geometry because there's no fixed-function pipeline overhead and no serialization (unlike GS).

### Interview Q&A — Section 7

**Q: Explain the top-left fill rule.**
A: When two triangles share an edge, the rasterizer needs a deterministic rule to decide which triangle "owns" pixels exactly on that edge, to avoid double-drawing or gaps. The top-left rule says: a pixel on a horizontal top edge belongs to that triangle, and a pixel on a left edge (a non-horizontal edge on the left side) belongs to that triangle. All other shared-edge pixels belong to the adjacent triangle. This is a spec requirement in both OpenGL and D3D, and getting it wrong in hardware produces visible seams between triangles. It's one of those "simple but critical" fixed-function behaviors that verification tests must cover.
*Follow-up:* How would you test this? — Render two adjacent triangles sharing an edge with different solid colors, at sub-pixel offsets that force samples onto the shared edge. Verify that every pixel along the edge belongs to exactly one triangle — no gaps (black pixels) and no double-draws (blended color). Repeat with different triangle orientations to test top vs left edge classification.

**Q: Why does reversed-Z improve depth precision?**
A: Standard projection maps the near plane to z=0 (or -1) and the far plane to z=1. But the 1/z transformation is highly nonlinear — almost all of the [0,1] range is consumed by objects close to the camera, and objects far away are crammed into a tiny range near 1.0. Floating-point has more precision near zero (because the exponent can represent smaller intervals), so this is a terrible match. Reversed-Z flips it: near→1.0, far→0.0. Now distant objects map to values near zero, where float precision is highest. The 1/z nonlinearity and the float precision curve cancel out, giving roughly uniform depth resolution across the entire range. You need a [0,1] depth range for this to work, which is why `glClipControl` was added to GL.
*Follow-up:* Does reversed-Z require any shader changes? — No. You only change the projection matrix and the depth comparison function (from `GL_LESS` to `GL_GREATER`). The vertex shader and fragment shader are unchanged.

**Q: Why is order-independent transparency hard?**
A: Because alpha blending is non-commutative: blending fragment A over B gives a different result than B over A. With opaque geometry, the depth buffer resolves which fragment is closest regardless of draw order. But transparent fragments can't just use depth testing — you need to see through them. The correct approach is to draw all transparent geometry sorted back-to-front, but sorting per-pixel is expensive, and intersecting transparent surfaces make a global sort impossible. Approximate solutions like weighted blended OIT sacrifice accuracy for speed. Exact solutions like per-pixel linked lists require unbounded memory. This remains an unsolved problem in real-time graphics.
*Follow-up:* How does this affect testing? — You must test blending with known draw order and compare against a golden computed with the same order. Random draw order will produce valid but different results, making comparison meaningless. This is why deterministic draw order matters in a test framework.

**Q: Explain the sRGB color space and why it matters for rendering.**
A: sRGB is a nonlinear encoding that allocates more bits to dark values, matching human perception. If you do lighting math in sRGB space, you get incorrect results — adding two sRGB values doesn't give the correct physical sum of light. The correct pipeline: read textures in sRGB, convert to linear on fetch (the texture unit does this automatically with sRGB formats), do all math in linear space, then convert back to sRGB on framebuffer write. If you skip the conversions, blending and lighting look wrong — overly dark shadows, incorrect color mixing. In verification, you need to know whether your comparison is in linear or sRGB space, because a ±1 LSB tolerance in sRGB means different things in different brightness ranges.
*Follow-up:* What happens if the framebuffer is linear but the display expects sRGB? — The image appears washed out — dark areas are too bright because the display applies its own sRGB curve on top. Desktop compositors generally assume sRGB output. In HDR workflows, the situation is more complex with PQ/HLG transfer functions.

**Q: What is a shader binding table in the ray tracing pipeline?**
A: The SBT is a GPU-side table that maps each combination of (geometry instance, ray type) to the shader programs that should execute when a ray hits that geometry. It has sections for ray generation, miss, hit groups (closest-hit + any-hit + intersection), and callable shaders. When the hardware finishes a ray-BVH traversal and finds an intersection, it looks up the SBT entry for that geometry instance and ray type to determine which closest-hit shader to invoke. This is essentially a vtable for ray interactions — it decouples geometry from shading, allowing different materials per instance without branching in a single shader. Building the SBT correctly is one of the trickier parts of RT pipeline setup.
*Follow-up:* How does the SBT index get computed? — The index is: `ray_contribution_to_hit_group_index + geometry_index × sbt_record_stride + instance_sbt_offset`. The instance offset comes from the acceleration structure build, the geometry index is implicit from the BVH leaf, and the ray contribution comes from the `TraceRay` call. This formula lets you organize shaders by material and ray type simultaneously.

---

## Red Flags

These are the things that will sink your interview if you say them:

1. **"The vertex shader outputs NDC coordinates."** No — it outputs **clip space** (vec4). The perspective divide happens after clipping. This distinction matters because clipping is done in homogeneous coordinates.

2. **"Early-Z always works."** No — it's disabled by `discard`, `gl_FragDepth` writes, alpha test, and certain stencil configurations. You must know the disablers.

3. **"MSAA runs the fragment shader per sample."** No — by default, MSAA runs the shader per **pixel** and broadcasts the result to covered samples. Per-sample shading only happens when the shader uses `gl_SampleID` or `sample`-qualified interpolation.

4. **"Vulkan is faster than OpenGL."** Not inherently — Vulkan gives you the *tools* for better performance (predictable compilation, multithreaded command recording, explicit sync), but a naive Vulkan app can be slower than a well-optimized GL app with a good driver. The win is predictability, not magic speed.

5. **"Triangles are independent — each one is fully processed before the next."** No — the GPU is a massively parallel pipeline. Thousands of triangles and millions of fragments are in-flight simultaneously across different stages.

6. **"Compute shaders and CUDA kernels use different hardware."** No — same SMs, same registers, same shared memory. Different host-side API and compiler front-end, same backend.

7. **"The rasterizer generates one fragment per pixel."** It generates fragments per **sample** (for MSAA), and it dispatches them in **2x2 quads**, including helper lanes for uncovered pixels.

8. **"Blending is commutative."** Alpha blending is not. Draw order matters for transparency. Only additive blending is commutative.

9. **"Depth buffers use linear depth."** Standard projection gives 1/z depth distribution — highly nonlinear. This is why reversed-Z exists.

10. **"Synchronization in Vulkan is optional for correctness."** No — missing barriers cause **undefined behavior**, typically manifesting as data corruption, stale cache reads, or wrong image layouts. Validation layers catch most issues during development.

---

## Whiteboard Checklist

When you're at the whiteboard, be ready to draw and explain each of these from memory:

- [ ] **Draw the full graphics pipeline** from Input Assembly through ROP/framebuffer write. Label each stage as programmable or fixed-function. Include both the early-Z and late-Z paths.

- [ ] **Draw the coordinate space chain:** Object → (Model) → World → (View) → Eye → (Projection) → Clip → (/w) → NDC → (Viewport) → Window. State the NDC ranges for GL, Vulkan, and D3D. Note the Y-axis flip in Vulkan.

- [ ] **Draw a 2x2 quad** showing helper lanes at a triangle edge. Explain why quads are mandatory (derivatives for mipmap LOD). Show the `dFdx`/`dFdy` computation.

- [ ] **Draw the early-Z vs late-Z decision flow.** List all early-Z disablers: `gl_FragDepth` write, `discard`, alpha test, certain stencil ops, shader side effects. Explain Hi-Z as a coarse pre-filter.

- [ ] **Draw the Vulkan object hierarchy:** Instance → PhysicalDevice → Device → Queue → CommandPool → CommandBuffer. Show the parallel branch: Device → Memory → Buffer/Image. Show Pipeline ← PipelineLayout ← DescriptorSetLayout.

- [ ] **List Vulkan synchronization primitives:**
  - **Fence:** CPU waits for GPU. Frame-level sync.
  - **Semaphore:** GPU→GPU between submissions. Swapchain acquire/present.
  - **Timeline Semaphore:** Counter-based, multi-point. Flexible dependency graphs.
  - **Event:** Fine-grained GPU-side signal/wait. Partial pipeline sync.
  - **Pipeline Barrier:** Memory + execution dependency within a command buffer. Image layout transitions.

- [ ] **Explain reversed-Z** with the precision curve. Draw the 1/z mapping and show how standard Z wastes precision on near objects while reversed-Z distributes it evenly.

- [ ] **Describe the shader compilation pipeline:** GLSL/HLSL → SPIR-V/DXIL → Vendor IR → ISA. Explain where specialization constants and pipeline caches fit in.

- [ ] **Explain how a graphics verification test differs from a game workload:** deterministic state, known expected output, isolation of features, corner case coverage, golden image or CRC comparison.

- [ ] **Draw the blending pipeline:** src color × src factor **OP** dst color × dst factor → result. Explain why alpha blending requires back-to-front ordering for correctness.
