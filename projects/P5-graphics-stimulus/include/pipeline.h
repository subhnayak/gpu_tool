/*
 * pipeline.h — Software Graphics Pipeline with Explicit, Testable Stages
 * =========================================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand every stage of the graphics pipeline, in order
 *   - Know which stages are fixed-function vs programmable on real GPUs
 *   - Be able to walk through the pipeline on a whiteboard (THE classic interview)
 *
 * PIPELINE STAGES (in order):
 *   1. Input Assembly (IA)     — FIXED-FUNCTION: reads vertex/index buffers
 *   2. Vertex Shader (VS)      — PROGRAMMABLE: transforms vertices
 *   3. Clipping                 — FIXED-FUNCTION: clips to frustum
 *   4. Perspective Divide       — FIXED-FUNCTION: clip → NDC
 *   5. Viewport Transform       — FIXED-FUNCTION: NDC → screen coords
 *   6. Backface Culling         — FIXED-FUNCTION (configurable): reject back-facing tris
 *   7. Triangle Setup           — FIXED-FUNCTION: compute edge equations
 *   8. Rasterization            — FIXED-FUNCTION: find covered pixels/fragments
 *   9. Early-Z Test             — FIXED-FUNCTION (optional): reject occluded fragments early
 *  10. Fragment Shader (FS)     — PROGRAMMABLE: compute fragment color
 *  11. Late Depth/Stencil Test  — FIXED-FUNCTION: final depth/stencil check
 *  12. Blending                 — FIXED-FUNCTION (configurable): blend with framebuffer
 *  13. Framebuffer Write        — FIXED-FUNCTION: write final color
 *
 * Note: Modern GPUs also have geometry, tessellation, and mesh shaders between
 * VS and rasterization. We omit these for clarity — the core pipeline above is
 * what's universal.
 *
 * INTERVIEW: "Which pipeline stages are programmable?"
 * VS, HS (tessellation), DS (tessellation), GS (geometry), FS. The rest are
 * fixed-function but often configurable (e.g., blend mode, cull mode, depth func).
 */

#ifndef PIPELINE_H
#define PIPELINE_H

#include "math3d.h"
#include "framebuffer.h"
#include "texture.h"

#include <vector>
#include <functional>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * VERTEX AND FRAGMENT DATA
 * ═══════════════════════════════════════════════════════════════════════
 * In real GPUs, vertex outputs and fragment inputs are passed through
 * "varyings" — interpolated values. Modern GPUs allocate these from a
 * parameter cache. We use a fixed set of attributes for simplicity.
 */
static const int MAX_ATTRIBS = 8;

struct Vertex {
    Vec4 position;      // object-space position (input to VS)
    float attribs[MAX_ATTRIBS] = {}; // generic attributes (color, texcoord, normal, etc.)
};

/* The output of the vertex shader — position in CLIP SPACE plus varyings. */
struct VSOutput {
    Vec4 clipPos;       // position in clip space (after projection)
    float attribs[MAX_ATTRIBS] = {}; // varyings to interpolate across the triangle
};

/* Fragment data passed to the fragment shader. */
struct Fragment {
    float x, y;         // screen-space position (pixel center)
    float depth;        // interpolated depth for depth testing
    float attribs[MAX_ATTRIBS] = {}; // interpolated varyings

    /* Screen-space derivatives for texture LOD, computed from the 2x2 quad.
     * dFdx[i] = attrib[i] of (x+1,y) - attrib[i] of (x,y)
     * dFdy[i] = attrib[i] of (x,y+1) - attrib[i] of (x,y)  */
    float dFdx[MAX_ATTRIBS] = {};
    float dFdy[MAX_ATTRIBS] = {};
};

/* Fragment shader output. */
struct FragOutput {
    Color color;
    bool discard = false; // if true, this fragment is killed (like GLSL discard)
};

/* ═══════════════════════════════════════════════════════════════════════
 * PIPELINE CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════ */

/* Primitive topology — how vertices are assembled into primitives.
 * INTERVIEW: "What topologies does D3D12/Vulkan support?"
 * Point list, line list, line strip, triangle list, triangle strip,
 * triangle fan (GL only), plus adjacency variants and patch lists
 * (for tessellation). */
enum class Topology { TRIANGLE_LIST, TRIANGLE_STRIP };

/* Winding order for front-face determination.
 * GL default: CCW is front. D3D default: CW is front.
 * Vulkan: configurable, but default is CCW after Y-flip. */
enum class FrontFace { CCW, CW };

/* Cull mode: which faces to discard. */
enum class CullMode { NONE, FRONT, BACK };

/* Depth comparison function. */
enum class DepthFunc { ALWAYS, NEVER, LESS, LESS_EQUAL, GREATER, GREATER_EQUAL, EQUAL };

/* Blend mode — simplified. Real APIs have separate src/dst/op for color and alpha. */
enum class BlendMode { NONE, ALPHA, ADDITIVE };

/* Depth range convention */
enum class DepthRange { GL_NEG1_TO_1, VK_0_TO_1 };

/* Shader function types — std::function for flexibility.
 * Real GPUs execute these as compiled shader programs on SIMD cores. */
using VertexShaderFn = std::function<VSOutput(const Vertex&)>;
using FragmentShaderFn = std::function<FragOutput(const Fragment&)>;

/* ═══════════════════════════════════════════════════════════════════════
 * PIPELINE STATE
 * ═══════════════════════════════════════════════════════════════════════
 * In Vulkan/D3D12, this is a "Pipeline State Object" (PSO). Changing it
 * is expensive because the GPU must flush caches and reconfigure hardware.
 * This is why modern APIs batch draws by PSO.
 */
struct PipelineState {
    VertexShaderFn vertexShader;
    FragmentShaderFn fragmentShader;

    Topology topology = Topology::TRIANGLE_LIST;
    FrontFace frontFace = FrontFace::CCW;
    CullMode cullMode = CullMode::BACK;
    DepthFunc depthFunc = DepthFunc::LESS;
    bool depthWrite = true;
    bool earlyZ = true;        // enable early-Z optimization
    BlendMode blendMode = BlendMode::NONE;
    DepthRange depthRange = DepthRange::GL_NEG1_TO_1;

    Viewport viewport;
    int numAttribs = 0;  // how many attribs to interpolate
};

/* ═══════════════════════════════════════════════════════════════════════
 * CLIPPING — Sutherland-Hodgman Polygon Clipping
 * ═══════════════════════════════════════════════════════════════════════
 *
 * INTERVIEW: "How does hardware clip triangles against the frustum?"
 * Sutherland-Hodgman clips a polygon against one plane at a time.
 * A triangle clipped against a plane can produce 3-4 vertices (a quad).
 * After clipping against all 6 planes, we re-triangulate the result.
 *
 * In clip space, the frustum is defined by:
 *   -w <= x <= w
 *   -w <= y <= w
 *   -w <= z <= w    (GL) or 0 <= z <= w (Vulkan/D3D)
 *
 * Modern GPUs often use a "guard band" — an enlarged clip region in X/Y
 * that avoids clipping most triangles. Only the near/far planes require
 * actual clipping; X/Y clipping is deferred to rasterization. This is
 * because clipping is expensive (generates new vertices) while
 * rasterization trivially handles off-screen triangles.
 */
namespace clip {

/* Clip against one plane. The plane is defined by:
 *   dot(vertex.clipPos, plane) >= 0 means "inside"
 *
 * The six frustum planes in clip space are:
 *   Left:   w + x >= 0   → plane = ( 1, 0, 0, 1)
 *   Right:  w - x >= 0   → plane = (-1, 0, 0, 1)
 *   Bottom: w + y >= 0   → plane = ( 0, 1, 0, 1)
 *   Top:    w - y >= 0   → plane = ( 0,-1, 0, 1)
 *   Near:   w + z >= 0   → plane = ( 0, 0, 1, 1)   [GL: z >= -w]
 *   Far:    w - z >= 0   → plane = ( 0, 0,-1, 1)   [GL: z <=  w]
 *   Near(VK): z >= 0     → plane = ( 0, 0, 1, 0)
 */
inline float planeDist(const VSOutput& v, const Vec4& plane) {
    return plane.x * v.clipPos.x + plane.y * v.clipPos.y +
           plane.z * v.clipPos.z + plane.w * v.clipPos.w;
}

/* Linearly interpolate between two VSOutput vertices at parameter t. */
inline VSOutput lerpVS(const VSOutput& a, const VSOutput& b, float t, int numAttribs) {
    VSOutput result;
    result.clipPos = a.clipPos * (1.0f - t) + b.clipPos * t;
    for (int i = 0; i < numAttribs; ++i)
        result.attribs[i] = a.attribs[i] * (1.0f - t) + b.attribs[i] * t;
    return result;
}

/* Clip a polygon (as a vector of VSOutput) against a single plane.
 * Returns the clipped polygon. Classic Sutherland-Hodgman algorithm. */
inline std::vector<VSOutput> clipAgainstPlane(const std::vector<VSOutput>& poly,
                                               const Vec4& plane, int numAttribs) {
    if (poly.empty()) return {};
    std::vector<VSOutput> out;
    size_t n = poly.size();

    for (size_t i = 0; i < n; ++i) {
        const VSOutput& cur = poly[i];
        const VSOutput& prev = poly[(i + n - 1) % n];
        float dc = planeDist(cur, plane);
        float dp = planeDist(prev, plane);
        bool curInside = dc >= 0;
        bool prevInside = dp >= 0;

        if (curInside != prevInside) {
            /* Edge crosses the plane — compute intersection */
            float t = dp / (dp - dc);
            out.push_back(lerpVS(prev, cur, t, numAttribs));
        }
        if (curInside) {
            out.push_back(cur);
        }
    }
    return out;
}

/* Clip against all 6 frustum planes. Returns clipped polygon (may be empty,
 * triangle, quad, or larger polygon). The caller must re-triangulate. */
inline std::vector<VSOutput> clipTriangle(const VSOutput& v0, const VSOutput& v1,
                                           const VSOutput& v2, int numAttribs,
                                           DepthRange depthRange) {
    std::vector<VSOutput> poly = {v0, v1, v2};

    /* Six clip planes in clip space */
    Vec4 planes[6] = {
        { 1,  0,  0, 1},   // left:   x >= -w
        {-1,  0,  0, 1},   // right:  x <=  w
        { 0,  1,  0, 1},   // bottom: y >= -w
        { 0, -1,  0, 1},   // top:    y <=  w
        { 0,  0,  0, 0},   // near:   set below
        { 0,  0, -1, 1},   // far:    z <=  w
    };

    /* Near plane depends on depth convention */
    if (depthRange == DepthRange::GL_NEG1_TO_1)
        planes[4] = {0, 0, 1, 1};   // z >= -w
    else
        planes[4] = {0, 0, 1, 0};   // z >= 0

    for (int i = 0; i < 6; ++i) {
        poly = clipAgainstPlane(poly, planes[i], numAttribs);
        if (poly.empty()) return {};
    }
    return poly;
}

} // namespace clip

/* ═══════════════════════════════════════════════════════════════════════
 * THE PIPELINE — Main rendering function
 * ═══════════════════════════════════════════════════════════════════════
 * This implements the full pipeline: IA → VS → Clip → Divide → Viewport →
 * Cull → Setup → Rasterize → (Early-Z →) FS → Depth Test → Blend → Write
 *
 * The rasterizer itself (edge function, barycentric, fill rule, quad shading)
 * is in raster.cpp. This file orchestrates the stages.
 */

/* Forward declaration — implemented in raster.cpp */
void rasterizeTriangle(const Vec3 screenVerts[3], const VSOutput clipVerts[3],
                       const PipelineState& state, Framebuffer& fb);

/* Compare depth values according to the depth function. */
inline bool depthTest(float fragmentDepth, float bufferDepth, DepthFunc func) {
    switch (func) {
        case DepthFunc::ALWAYS:        return true;
        case DepthFunc::NEVER:         return false;
        case DepthFunc::LESS:          return fragmentDepth < bufferDepth;
        case DepthFunc::LESS_EQUAL:    return fragmentDepth <= bufferDepth;
        case DepthFunc::GREATER:       return fragmentDepth > bufferDepth;
        case DepthFunc::GREATER_EQUAL: return fragmentDepth >= bufferDepth;
        case DepthFunc::EQUAL:         return fragmentDepth == bufferDepth;
    }
    return false;
}

/* Alpha blending. Real GPUs have configurable src/dst factors and ops.
 * We implement the two most common modes. */
inline Color blendColor(const Color& src, const Color& dst, BlendMode mode) {
    if (mode == BlendMode::NONE) return src;

    float sr = src.r / 255.0f, sg = src.g / 255.0f, sb = src.b / 255.0f, sa = src.a / 255.0f;
    float dr = dst.r / 255.0f, dg = dst.g / 255.0f, db = dst.b / 255.0f, da = dst.a / 255.0f;

    float or_, og, ob, oa;
    if (mode == BlendMode::ALPHA) {
        /* Standard alpha blend: out = src * srcAlpha + dst * (1 - srcAlpha) */
        or_ = sr * sa + dr * (1.0f - sa);
        og = sg * sa + dg * (1.0f - sa);
        ob = sb * sa + db * (1.0f - sa);
        oa = sa + da * (1.0f - sa);
    } else { // ADDITIVE
        or_ = sr + dr; og = sg + dg; ob = sb + db; oa = sa + da;
    }
    return Color::fromFloat(or_, og, ob, oa);
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN DRAW CALL — drawTriangles()
 * ═══════════════════════════════════════════════════════════════════════
 * This is the equivalent of glDrawElements / vkCmdDrawIndexed.
 *
 * INTERVIEW: "Walk me through what happens when I call vkCmdDraw."
 * (This function IS the answer.)
 */
inline void drawTriangles(const std::vector<Vertex>& vertices,
                          const std::vector<uint32_t>& indices,
                          const PipelineState& state,
                          Framebuffer& fb) {
    /* ─── Stage 1: Input Assembly ───────────────────────────────────────
     * HARDWARE: The Input Assembler reads vertex data from buffers in GPU
     * memory, fetches indices from the index buffer, and assembles vertices
     * into primitives (triangles, lines, points). It also provides the
     * vertex ID and instance ID as system values.
     *
     * On real GPUs, there's a "post-transform vertex cache" that avoids
     * re-running the vertex shader for vertices shared between triangles
     * (common with indexed rendering). */

    size_t numIndices = indices.empty() ? vertices.size() : indices.size();
    int stride = (state.topology == Topology::TRIANGLE_LIST) ? 3 : 1;

    for (size_t i = 0; i + 2 < numIndices; i += stride) {
        size_t i0, i1, i2;
        if (state.topology == Topology::TRIANGLE_LIST) {
            i0 = i; i1 = i + 1; i2 = i + 2;
        } else {
            /* Triangle strip: alternating winding for consistent face direction.
             * This is a hardware optimization — reduces index buffer size. */
            i0 = i; i1 = i + 1; i2 = i + 2;
            if (i % 2 == 1) std::swap(i1, i2);
        }

        uint32_t idx0 = indices.empty() ? (uint32_t)i0 : indices[i0];
        uint32_t idx1 = indices.empty() ? (uint32_t)i1 : indices[i1];
        uint32_t idx2 = indices.empty() ? (uint32_t)i2 : indices[i2];

        /* ─── Stage 2: Vertex Shader ────────────────────────────────────
         * HARDWARE: Runs on shader cores (SMs on NVIDIA). Each vertex is
         * processed by one thread. The VS transforms the position (typically
         * by MVP matrix) and passes along varyings (color, texcoord, etc.).
         * Output: position in CLIP SPACE (4D homogeneous coordinates). */
        VSOutput vs0 = state.vertexShader(vertices[idx0]);
        VSOutput vs1 = state.vertexShader(vertices[idx1]);
        VSOutput vs2 = state.vertexShader(vertices[idx2]);

        /* ─── Stage 3: Clipping ─────────────────────────────────────────
         * HARDWARE: Fixed-function clipping against the view frustum.
         * Uses guard bands for X/Y (defer clipping to rasterizer), but
         * MUST clip against the near plane (otherwise perspective divide
         * produces garbage for vertices behind the camera).
         *
         * Clipping can turn one triangle into multiple (up to... many).
         * We clip and then fan-triangulate the result polygon. */
        std::vector<VSOutput> clipped = clip::clipTriangle(
            vs0, vs1, vs2, state.numAttribs, state.depthRange);

        if (clipped.size() < 3) continue; // fully clipped

        /* Fan-triangulate the clipped polygon.
         * A polygon with N vertices produces N-2 triangles. */
        for (size_t t = 1; t + 1 < clipped.size(); ++t) {
            VSOutput tri[3] = {clipped[0], clipped[t], clipped[t + 1]};

            /* ─── Stage 4: Perspective Divide ───────────────────────────
             * HARDWARE: Fixed-function. Divides x, y, z by w to produce
             * Normalized Device Coordinates (NDC).
             * x_ndc = x_clip / w_clip, etc.
             *
             * After this, coordinates are in the NDC cube:
             *   GL: [-1,+1] for x,y,z
             *   VK: [-1,+1] for x,y; [0,1] for z */
            Vec3 ndc[3];
            for (int v = 0; v < 3; ++v) {
                if (tri[v].clipPos.w == 0.0f) goto next_tri; // degenerate
                ndc[v] = tri[v].clipPos.perspDivide();
            }

            {
            /* ─── Stage 5: Viewport Transform ──────────────────────────
             * HARDWARE: Fixed-function. Maps NDC to pixel coordinates.
             * See Viewport::transform() in math3d.h. */
            Vec3 screen[3];
            for (int v = 0; v < 3; ++v)
                screen[v] = state.viewport.transform(ndc[v]);

            /* ─── Stage 6: Backface Culling ────────────────────────────
             * HARDWARE: Fixed-function, configurable. Computes the signed
             * area of the triangle in screen space. If negative (CW) or
             * positive (CCW) depending on configuration, the triangle is
             * back-facing and can be culled.
             *
             * Signed area = 0.5 * ((x1-x0)*(y2-y0) - (x2-x0)*(y1-y0))
             * We don't need the 0.5 — only the sign matters.
             *
             * INTERVIEW: "How does the GPU determine front vs back face?"
             * By the sign of the signed area in screen space after
             * projection. This is determined by the winding order of the
             * vertices (CW or CCW) combined with the frontFace setting. */
            float signedArea2x = (screen[1].x - screen[0].x) * (screen[2].y - screen[0].y) -
                                 (screen[2].x - screen[0].x) * (screen[1].y - screen[0].y);

            /* Zero area = degenerate triangle. Skip it. */
            if (signedArea2x == 0.0f) goto next_tri;

            bool isCCW = signedArea2x > 0.0f;
            bool isFrontFace = (state.frontFace == FrontFace::CCW) ? isCCW : !isCCW;

            if (state.cullMode == CullMode::BACK && !isFrontFace) goto next_tri;
            if (state.cullMode == CullMode::FRONT && isFrontFace) goto next_tri;

            /* ─── Stages 7-13: Rasterization through framebuffer write ─
             * These are implemented in raster.cpp. The rasterizer handles:
             *   7. Triangle Setup: compute edge equations
             *   8. Rasterization: edge function, fill rule, pixel coverage
             *   9. Early-Z: optional depth test before fragment shader
             *  10. Fragment Shader: per-fragment color computation
             *  11. Late Depth/Stencil: final depth test
             *  12. Blending: combine fragment color with framebuffer
             *  13. Write: store result to framebuffer */
            rasterizeTriangle(screen, tri, state, fb);
            }

        next_tri:;
        }
    }
}

} // namespace softras

#endif // PIPELINE_H
