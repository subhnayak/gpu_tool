/*
 * shaders.cpp — Example Vertex and Fragment Shader Callables
 * ============================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand what vertex and fragment shaders do
 *   - See concrete examples mapping to GLSL/HLSL concepts
 *   - Understand how derivatives are used for texture LOD
 *
 * These are C++ callables (std::function) that act as shaders.
 * On real GPUs, shaders are compiled to GPU-specific ISA (PTX→SASS
 * on NVIDIA, DXIL→ISA on AMD, SPIR-V→ISA on all Vulkan drivers).
 *
 * Each shader here is commented with what the equivalent GLSL would look like.
 */

#include "pipeline.h"

namespace softras {
namespace shaders {

/* ═══════════════════════════════════════════════════════════════════════
 * VERTEX SHADERS
 * ═══════════════════════════════════════════════════════════════════════
 * All vertex shaders must:
 *   1. Transform the position by the MVP matrix → clipPos
 *   2. Pass through (or compute) varyings
 *
 * Attribute convention for all our shaders:
 *   attribs[0..2] = vertex color (r, g, b)
 *   attribs[3..4] = texture coordinates (u, v)
 *   attribs[5..7] = normal (nx, ny, nz)
 */

/* ─── Pass-through VS: applies MVP, passes all attributes unchanged ── */
/* GLSL equivalent:
 *   gl_Position = u_MVP * a_Position;
 *   v_Color = a_Color;
 *   v_TexCoord = a_TexCoord;
 */
inline VertexShaderFn makePassthroughVS(const Mat4& mvp, int numAttribs = 5) {
    return [=](const Vertex& v) -> VSOutput {
        VSOutput out;
        out.clipPos = mvp * v.position;
        for (int i = 0; i < numAttribs && i < MAX_ATTRIBS; ++i)
            out.attribs[i] = v.attribs[i];
        return out;
    };
}

/* ═══════════════════════════════════════════════════════════════════════
 * FRAGMENT SHADERS
 * ═══════════════════════════════════════════════════════════════════════ */

/* ─── Flat Color FS: every fragment gets the same color ────────────── */
/* GLSL equivalent:
 *   out vec4 fragColor;
 *   void main() { fragColor = u_Color; }
 */
inline FragmentShaderFn makeFlatColorFS(const Color& c) {
    return [=](const Fragment&) -> FragOutput {
        return {c, false};
    };
}

/* ─── Vertex Color FS: interpolated per-vertex colors ─────────────── */
/* GLSL equivalent:
 *   in vec3 v_Color;
 *   void main() { fragColor = vec4(v_Color, 1.0); }
 *
 * INTERVIEW: "How are varyings interpolated across a triangle?"
 * With perspective-correct interpolation. The GPU interpolates
 * attr/w and 1/w linearly, then divides. See raster.cpp for details.
 */
inline FragmentShaderFn vertexColorFS() {
    return [](const Fragment& f) -> FragOutput {
        return {Color::fromFloat(f.attribs[0], f.attribs[1], f.attribs[2]), false};
    };
}

/* ─── Textured FS: sample a texture using interpolated UVs ─────────── */
/* GLSL equivalent:
 *   in vec2 v_TexCoord;
 *   uniform sampler2D u_Texture;
 *   void main() { fragColor = texture(u_Texture, v_TexCoord); }
 *
 * The `texture()` call in GLSL implicitly uses screen-space derivatives
 * (dFdx/dFdy of the texture coordinates) to select the mip level.
 * Our Fragment struct carries these derivatives from quad processing.
 */
inline FragmentShaderFn makeTexturedFS(const Texture* tex) {
    return [tex](const Fragment& f) -> FragOutput {
        float u = f.attribs[3];
        float v = f.attribs[4];

        /* Use derivatives for LOD selection */
        TexColor tc = tex->sampleWithDerivatives(
            u, v,
            f.dFdx[3], f.dFdx[4],  // du/dx, dv/dx
            f.dFdy[3], f.dFdy[4],  // du/dy, dv/dy
            true /* bilinear */
        );
        return {Color::fromFloat(tc.r, tc.g, tc.b, tc.a), false};
    };
}

/* ─── Depth Visualization FS: maps depth to grayscale ──────────────── */
/* Useful for debugging depth buffer contents and verifying depth precision.
 *
 * INTERVIEW: "How do you visualize the depth buffer?"
 * Map [near, far] → [white, black] (or use a color ramp). Non-linear
 * mapping shows the non-linear distribution of depth precision —
 * most precision is near the camera with standard projection. */
inline FragmentShaderFn makeDepthVisFS(float nearVal = 0.0f, float farVal = 1.0f) {
    return [=](const Fragment& f) -> FragOutput {
        float d = (f.depth - nearVal) / (farVal - nearVal);
        d = std::max(0.0f, std::min(1.0f, d));
        /* Invert so near=white, far=black (more intuitive) */
        float gray = 1.0f - d;
        return {Color::fromFloat(gray, gray, gray), false};
    };
}

/* ─── Derivative Visualization FS: shows dFdx/dFdy as colors ──────── */
/* This shader visualizes the screen-space derivatives of a chosen
 * attribute, which is exactly what the GPU uses for texture LOD and
 * other screen-space effects (SSAO, screen-space reflections, etc.).
 *
 * INTERVIEW: "How do screen-space derivatives work?"
 * See texture.h and raster.cpp. Computed per 2x2 quad using
 * finite differences between neighboring fragments.
 *
 * We map: R = |dFdx| * scale, G = |dFdy| * scale, B = 0 */
inline FragmentShaderFn makeDerivativeVisFS(int attribIdx = 3, float scale = 10.0f) {
    return [=](const Fragment& f) -> FragOutput {
        float dx = std::abs(f.dFdx[attribIdx]) * scale;
        float dy = std::abs(f.dFdy[attribIdx]) * scale;
        return {Color::fromFloat(dx, dy, 0.0f), false};
    };
}

/* ─── Checkerboard FS: procedural texture using floor() ────────────── */
/* Demonstrates aliasing without mipmapping — the checkerboard will
 * show moiré patterns at steep angles. With derivatives, we could
 * detect this and blend. */
inline FragmentShaderFn makeCheckerFS(float gridSize = 8.0f) {
    return [=](const Fragment& f) -> FragOutput {
        float u = f.attribs[3] * gridSize;
        float v = f.attribs[4] * gridSize;
        bool white = (((int)std::floor(u)) + ((int)std::floor(v))) % 2 == 0;
        float c = white ? 1.0f : 0.2f;
        return {Color::fromFloat(c, c, c), false};
    };
}

} // namespace shaders
} // namespace softras
