/*
 * raster.cpp — Edge-Function Rasterizer with Fill Rule and Quad Shading
 * =======================================================================
 *
 * LEARNING OBJECTIVES:
 *   - Implement barycentric/edge-function rasterization (how real GPUs work)
 *   - Correctly implement the TOP-LEFT FILL RULE (classic interview question)
 *   - Implement perspective-correct interpolation (another classic)
 *   - Implement 2x2 quad-based fragment processing with helper lanes
 *
 * INTERVIEW: "How does a GPU rasterize a triangle?"
 * Using the EDGE FUNCTION approach:
 *   1. For each edge of the triangle, define a function E(x,y) that is
 *      positive on one side, negative on the other, zero on the edge.
 *   2. A pixel is inside the triangle iff all three edge functions are >= 0
 *      (or <=0, depending on winding).
 *   3. The edge function values ARE the barycentric coordinates (unnormalized).
 *
 * This is how EVERY modern GPU rasterizer works. The edge function is:
 *   E_ij(x,y) = (x - xi)(yj - yi) - (y - yi)(xj - xi)
 * where (xi,yi) and (xj,yj) are the edge's two vertices.
 *
 * The edge function is linear in x and y, so it can be INCREMENTALLY EVALUATED:
 *   E(x+1, y) = E(x,y) + (yj - yi)     — step in x
 *   E(x, y+1) = E(x,y) - (xj - xi)     — step in y
 * This means the GPU only needs one addition per pixel per edge — extremely fast.
 * Real GPUs evaluate 8x8 or 16x16 pixel tiles in parallel using SIMD.
 */

#include "pipeline.h"
#include <cmath>
#include <algorithm>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * TOP-LEFT FILL RULE
 * ═══════════════════════════════════════════════════════════════════════
 *
 * WHY THIS MATTERS (interview gold!):
 * When two triangles share an edge, a pixel exactly ON that edge belongs
 * to EXACTLY ONE triangle. Without this rule, you get either:
 *   - Double-shading: the pixel is drawn twice (overdraw, wrong blending)
 *   - Gap: the pixel is drawn by neither triangle (visible crack)
 *
 * The rule: a pixel on an edge is included if the edge is a "top" edge
 * or a "left" edge:
 *   - TOP edge: perfectly horizontal, going LEFT (decreasing x)
 *   - LEFT edge: going UP (decreasing y)
 *
 * Why these specific edges? Because they're the edges that a "top-left"
 * pixel would naturally belong to. The choice is arbitrary but must be
 * consistent — this particular convention is what D3D, GL, and Vulkan
 * all use.
 *
 * IMPLEMENTATION: when the edge function is exactly zero (pixel on edge),
 * we include it only if the edge is top-left. For non-zero edge function
 * values, this doesn't apply — the pixel is clearly inside or outside.
 */

/* Check if an edge is a "top" or "left" edge for the fill rule.
 * Edge from (x0,y0) to (x1,y1):
 *   - Top edge: horizontal (dy == 0) and going left (dx < 0)
 *   - Left edge: going up (dy < 0)
 *
 * Note: in our screen space, Y increases downward, so "going up" means dy < 0. */
inline bool isTopLeftEdge(float dx, float dy) {
    if (dy < 0.0f) return true;                 // left edge (going up)
    if (dy == 0.0f && dx < 0.0f) return true;   // top edge (horizontal, going left)
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════
 * EDGE FUNCTION
 * ═══════════════════════════════════════════════════════════════════════
 * E(px, py) = (px - v0x) * (v1y - v0y) - (py - v0y) * (v1x - v0x)
 *
 * Positive = left of edge (inside for CCW triangle)
 * Zero = on the edge
 * Negative = right of edge (outside for CCW triangle)
 *
 * The three edge functions evaluated at a point give the (unnormalized)
 * barycentric coordinates of that point with respect to the triangle.
 */
inline float edgeFunction(float v0x, float v0y, float v1x, float v1y, float px, float py) {
    return (px - v0x) * (v1y - v0y) - (py - v0y) * (v1x - v0x);
}

/* ═══════════════════════════════════════════════════════════════════════
 * RASTERIZE A SINGLE TRIANGLE
 * ═══════════════════════════════════════════════════════════════════════
 *
 * This function implements stages 7-13 of the pipeline.
 *
 * Parameters:
 *   screenVerts[3]: screen-space positions (x, y in pixels, z = depth)
 *   clipVerts[3]:   the original VSOutput (for attributes and w for perspective correction)
 *   state:          pipeline configuration
 *   fb:             output framebuffer
 */
void rasterizeTriangle(const Vec3 screenVerts[3], const VSOutput clipVerts[3],
                       const PipelineState& state, Framebuffer& fb) {

    /* ─── Stage 7: Triangle Setup ──────────────────────────────────────
     * HARDWARE: Computes edge equations and attribute gradients.
     * The edge equations are E_i(x,y) = A_i*x + B_i*y + C_i.
     * These are set up once per triangle, then evaluated per pixel. */

    float x0 = screenVerts[0].x, y0 = screenVerts[0].y, z0 = screenVerts[0].z;
    float x1 = screenVerts[1].x, y1 = screenVerts[1].y, z1 = screenVerts[1].z;
    float x2 = screenVerts[2].x, y2 = screenVerts[2].y, z2 = screenVerts[2].z;

    /* Compute the signed area * 2 for the whole triangle.
     * This is also the edge function of vertex 2 with respect to edge 0→1. */
    float area2x = edgeFunction(x0, y0, x1, y1, x2, y2);
    if (area2x == 0.0f) return; // degenerate (zero-area) triangle

    /* If the triangle has negative area, it's wound CW in screen space.
     * We need positive area for our edge function math, so we could flip.
     * But we'll keep the original winding and adjust the sign check. */
    float sign = (area2x > 0.0f) ? 1.0f : -1.0f;
    float invArea = 1.0f / area2x;

    /* ─── Fill rule edge classification ─────────────────────────────── */
    /* Edge 0: v1 → v2, Edge 1: v2 → v0, Edge 2: v0 → v1 */
    bool tl0 = isTopLeftEdge(x2 - x1, y2 - y1);
    bool tl1 = isTopLeftEdge(x0 - x2, y0 - y2);
    bool tl2 = isTopLeftEdge(x1 - x0, y1 - y0);

    /* Apply fill rule bias: for non-top-left edges, exclude zero values.
     * We do this by adding a tiny bias so that exact-zero edge values
     * are treated as "outside" for non-top-left edges.
     * Note: Real hardware uses fixed-point and subtracts an ULP. */
    float bias0 = tl0 ? 0.0f : (sign > 0 ? -1e-5f : 1e-5f);
    float bias1 = tl1 ? 0.0f : (sign > 0 ? -1e-5f : 1e-5f);
    float bias2 = tl2 ? 0.0f : (sign > 0 ? -1e-5f : 1e-5f);

    /* ─── Bounding box (clipped to framebuffer) ────────────────────── */
    int minX = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
    int maxX = std::min(fb.width - 1, (int)std::ceil(std::max({x0, x1, x2})));
    int minY = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
    int maxY = std::min(fb.height - 1, (int)std::ceil(std::max({y0, y1, y2})));

    if (minX > maxX || minY > maxY) return;

    /* ─── Perspective-correct interpolation setup ──────────────────────
     *
     * INTERVIEW: "Why can't you linearly interpolate texture coordinates
     * in screen space?"
     *
     * Because perspective projection is a NON-LINEAR operation (divide by w).
     * If you linearly interpolate attributes in screen space, you get the
     * wrong values — textures will swim/warp incorrectly. This is exactly
     * what the PS1 did (it had no perspective correction), which is why
     * PS1 games have that characteristic "wobbly texture" look.
     *
     * THE FIX: Instead of interpolating attr, interpolate attr/w and 1/w.
     * Then at each pixel: attr = (interpolated attr/w) / (interpolated 1/w).
     *
     * Why does this work? Because 1/w varies linearly in screen space
     * (this is a mathematical property of perspective projection), and
     * attr/w also varies linearly. So linear interpolation of 1/w and
     * attr/w gives correct results, and dividing recovers the true attr.
     *
     * This is called "hyperbolic interpolation" and every GPU since the
     * late 1990s implements it. */

    float w0 = clipVerts[0].clipPos.w;
    float w1 = clipVerts[1].clipPos.w;
    float w2 = clipVerts[2].clipPos.w;

    /* Precompute 1/w for each vertex */
    float inv_w0 = 1.0f / w0;
    float inv_w1 = 1.0f / w1;
    float inv_w2 = 1.0f / w2;

    /* ─── Stage 8: Rasterization ───────────────────────────────────────
     * HARDWARE: The rasterizer iterates over the triangle's bounding box
     * (or, in modern GPUs, over tile-aligned blocks). For each pixel, it
     * evaluates the three edge functions to determine coverage.
     *
     * Modern GPUs process in 8x8 tiles: first do a coarse test on the
     * tile corners, skip tiles entirely outside, fully accept tiles
     * entirely inside, and fine-test edge pixels. This is called
     * "hierarchical rasterization."
     *
     * We process in 2x2 QUADS because we need screen-space derivatives.
     * Real GPUs also process in quads (the minimum unit of fragment work).
     */

    /* Align bounding box to 2x2 quads for quad processing */
    minX &= ~1; // round down to even
    minY &= ~1;

    for (int qy = minY; qy <= maxY; qy += 2) {
        for (int qx = minX; qx <= maxX; qx += 2) {
            /* ═══════════════════════════════════════════════════════════
             * 2x2 QUAD PROCESSING WITH HELPER LANES
             * ═══════════════════════════════════════════════════════════
             * Process 4 fragments as a quad:
             *   (qx, qy)     (qx+1, qy)
             *   (qx, qy+1)   (qx+1, qy+1)
             *
             * Even fragments outside the triangle or framebuffer are
             * processed as "helper lanes" — they compute attributes
             * so neighboring fragments can compute derivatives.
             *
             * INTERVIEW: "What is a helper lane?"
             * A fragment shader invocation that exists only to provide
             * derivative values to its neighbors in the 2x2 quad.
             * Helper lanes execute the shader but their results are
             * DISCARDED — they do NOT write to the framebuffer.
             * This is a source of wasted work, especially for small
             * triangles. */

            Fragment quadFrags[4]; // [0]=(qx,qy), [1]=(qx+1,qy), [2]=(qx,qy+1), [3]=(qx+1,qy+1)
            bool quadInside[4] = {};
            bool quadInBounds[4] = {};

            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int idx = dy * 2 + dx;
                    int px = qx + dx;
                    int py = qy + dy;

                    quadInBounds[idx] = (px >= 0 && px < fb.width && py >= 0 && py < fb.height);

                    /* Sample at pixel center (px + 0.5, py + 0.5).
                     * INTERVIEW: "Where is the sample point within a pixel?"
                     * At the center (0.5, 0.5) for non-MSAA rendering.
                     * MSAA has multiple sample points per pixel at
                     * jittered positions. */
                    float cx = px + 0.5f;
                    float cy = py + 0.5f;

                    /* Evaluate edge functions (barycentric coordinates) */
                    float e0 = edgeFunction(x1, y1, x2, y2, cx, cy) + bias0;
                    float e1 = edgeFunction(x2, y2, x0, y0, cx, cy) + bias1;
                    float e2 = edgeFunction(x0, y0, x1, y1, cx, cy) + bias2;

                    /* Inside test: all edge functions must have the same sign as the triangle area */
                    bool inside;
                    if (sign > 0)
                        inside = (e0 >= 0 && e1 >= 0 && e2 >= 0);
                    else
                        inside = (e0 <= 0 && e1 <= 0 && e2 <= 0);

                    quadInside[idx] = inside;

                    /* Compute barycentric coordinates (normalized) */
                    float b0 = e0 * invArea;
                    float b1 = e1 * invArea;
                    float b2 = e2 * invArea;

                    /* ─── Perspective-correct interpolation ─────────────
                     * Interpolate 1/w and attr/w, then divide.
                     * depth is interpolated linearly (it's already in
                     * screen space after the viewport transform). */
                    float interp_inv_w = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;
                    float perspW = 1.0f / interp_inv_w;

                    float depth = b0 * z0 + b1 * z1 + b2 * z2;

                    quadFrags[idx].x = cx;
                    quadFrags[idx].y = cy;
                    quadFrags[idx].depth = depth;

                    for (int a = 0; a < state.numAttribs; ++a) {
                        float av0 = clipVerts[0].attribs[a] * inv_w0;
                        float av1 = clipVerts[1].attribs[a] * inv_w1;
                        float av2 = clipVerts[2].attribs[a] * inv_w2;
                        float interp = b0 * av0 + b1 * av1 + b2 * av2;
                        quadFrags[idx].attribs[a] = interp * perspW;
                    }
                }
            }

            /* Check if any fragment in this quad is inside */
            bool anyInside = quadInside[0] || quadInside[1] || quadInside[2] || quadInside[3];
            if (!anyInside) continue;

            /* ─── Compute screen-space derivatives from the quad ────────
             *
             * dFdx = right - left  (within the 2x2 quad)
             * dFdy = bottom - top
             *
             * HARDWARE: This is done per-quad, using the register file.
             * Each lane in the quad can "read" its neighbor's value
             * through a special shuffle instruction (e.g., NVIDIA's
             * __shfl_sync in CUDA, or the "quad swizzle" instructions).
             *
             * We compute derivatives for ALL fragments in the quad,
             * including helpers (they need derivatives too, for their
             * own texture lookups that feed into LOD selection). */
            for (int a = 0; a < state.numAttribs; ++a) {
                /* dFdx: difference between right and left columns */
                float ddx_top = quadFrags[1].attribs[a] - quadFrags[0].attribs[a];
                float ddx_bot = quadFrags[3].attribs[a] - quadFrags[2].attribs[a];
                /* dFdy: difference between bottom and top rows */
                float ddy_left = quadFrags[2].attribs[a] - quadFrags[0].attribs[a];
                float ddy_right = quadFrags[3].attribs[a] - quadFrags[1].attribs[a];

                /* Each fragment uses the derivative from its half of the quad.
                 * Top row uses ddx_top, bottom row uses ddx_bot, etc. */
                quadFrags[0].dFdx[a] = ddx_top; quadFrags[0].dFdy[a] = ddy_left;
                quadFrags[1].dFdx[a] = ddx_top; quadFrags[1].dFdy[a] = ddy_right;
                quadFrags[2].dFdx[a] = ddx_bot; quadFrags[2].dFdy[a] = ddy_left;
                quadFrags[3].dFdx[a] = ddx_bot; quadFrags[3].dFdy[a] = ddy_right;
            }

            /* ─── Process each fragment in the quad ──────────────────── */
            for (int fi = 0; fi < 4; ++fi) {
                /* Skip helper lanes (they computed derivatives but don't write) */
                if (!quadInside[fi] || !quadInBounds[fi]) continue;

                int px = (int)(quadFrags[fi].x - 0.5f);
                int py = (int)(quadFrags[fi].y - 0.5f);
                float fragDepth = quadFrags[fi].depth;

                /* ─── Stage 9: Early-Z Test ────────────────────────────
                 * HARDWARE: Optional optimization. Tests depth BEFORE the
                 * fragment shader runs. If the fragment is behind existing
                 * geometry, skip the (expensive) shader entirely.
                 *
                 * INTERVIEW: "When can't the GPU do early-Z?"
                 * When the fragment shader:
                 *   - Writes to gl_FragDepth (modifies depth)
                 *   - Uses discard/clip (may not actually write)
                 *   - Has side effects (UAV writes, atomics)
                 * In these cases, the GPU falls back to late-Z testing. */
                if (state.earlyZ && !state.fragmentShader) {
                    if (!depthTest(fragDepth, fb.depthAt(px, py), state.depthFunc))
                        continue;
                }
                if (state.earlyZ && state.fragmentShader) {
                    /* We can do early-Z only if the shader doesn't discard
                     * or modify depth. For simplicity, we speculatively
                     * do early-Z and then also do late-Z. Real GPUs track
                     * this with shader metadata. */
                    if (!depthTest(fragDepth, fb.depthAt(px, py), state.depthFunc))
                        continue;
                }

                /* ─── Stage 10: Fragment Shader ────────────────────────
                 * HARDWARE: Runs on shader cores. Receives interpolated
                 * varyings, texture coordinates, and derivatives.
                 * Outputs a color (and optionally a modified depth).
                 * This is the most expensive stage for complex shaders. */
                FragOutput fragOut;
                if (state.fragmentShader) {
                    fragOut = state.fragmentShader(quadFrags[fi]);
                } else {
                    /* Default: white */
                    fragOut.color = Color(255, 255, 255, 255);
                }

                /* Handle discard (GLSL `discard` / HLSL `clip`) */
                if (fragOut.discard) continue;

                /* ─── Stage 11: Late Depth/Stencil Test ────────────────
                 * HARDWARE: Final depth test after the fragment shader.
                 * Always runs (early-Z is an optimization that may skip it).
                 * Also handles stencil test (not implemented here). */
                if (!depthTest(fragDepth, fb.depthAt(px, py), state.depthFunc))
                    continue;

                /* ─── Stage 12: Blending ───────────────────────────────
                 * HARDWARE: Fixed-function blend unit. Reads the current
                 * framebuffer color, blends with the fragment color, and
                 * writes back. The blend equation and factors are
                 * configurable via the API.
                 *
                 * INTERVIEW: "Why is blending order-dependent?"
                 * Because alpha blending is NOT commutative:
                 *   (A over B) ≠ (B over A)
                 * This means translucent objects MUST be drawn back-to-front.
                 * This is called the "painter's algorithm" and is one of
                 * the hardest problems in real-time rendering. Modern
                 * solutions include OIT (Order-Independent Transparency)
                 * using per-pixel linked lists or weighted-average
                 * techniques. */
                Color finalColor;
                if (state.blendMode != BlendMode::NONE) {
                    finalColor = blendColor(fragOut.color, fb.colorAt(px, py), state.blendMode);
                } else {
                    finalColor = fragOut.color;
                }

                /* ─── Stage 13: Framebuffer Write ──────────────────────
                 * HARDWARE: Write the final color and (optionally) depth
                 * to the framebuffer. On real GPUs, this goes through
                 * the ROP (Render Output Unit) which handles compression
                 * and cache management. */
                fb.colorAt(px, py) = finalColor;
                if (state.depthWrite)
                    fb.depthAt(px, py) = fragDepth;
            }
        }
    }
}

} // namespace softras
