/*
 * stimulus_main.cpp — Deterministic Rendering Stimulus Generator CLI
 * =====================================================================
 *
 * This is NOT a demo — it's a STIMULUS GENERATOR for GPU verification.
 * Each scene exercises specific hardware corner cases.
 *
 * Usage:
 *   stimulus run --scene N [--seed S] [--out img.ppm] [--hash-only]
 *   stimulus --list-scenes
 *
 * For each scene, we:
 *   1. Construct geometry targeting a specific hardware behavior
 *   2. Render it with the software rasterizer
 *   3. Output the image and/or its CRC32 hash
 *   4. The hash is the "golden" value — any change indicates a regression
 */

#include "pipeline.h"
#include "framebuffer.h"
#include "texture.h"
#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <iomanip>

/* Include the shaders — they're header-like with inline functions */
namespace softras { namespace shaders {
    /* Forward declarations from shaders.cpp (which has inline defs) */
    VertexShaderFn makePassthroughVS(const Mat4& mvp, int numAttribs);
    FragmentShaderFn makeFlatColorFS(const Color& c);
    FragmentShaderFn vertexColorFS();
    FragmentShaderFn makeDepthVisFS(float nearVal, float farVal);
    FragmentShaderFn makeDerivativeVisFS(int attribIdx, float scale);
    FragmentShaderFn makeTexturedFS(const Texture* tex);
    FragmentShaderFn makeCheckerFS(float gridSize);
}}

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * SCENE DEFINITIONS — Each exercises a specific hardware behavior
 * ═══════════════════════════════════════════════════════════════════════ */

struct Scene {
    int id;
    const char* name;
    const char* description;        // what hardware behavior it exercises
    std::function<void(Framebuffer&)> render;
};

static const int FB_W = 256;
static const int FB_H = 256;

/* Helper: make a vertex with position and color */
static Vertex makeVert(float x, float y, float z, float r, float g, float b) {
    Vertex v;
    v.position = Vec4(x, y, z, 1.0f);
    v.attribs[0] = r; v.attribs[1] = g; v.attribs[2] = b;
    return v;
}

/* Helper: make a vertex with position, color, and UV */
static Vertex makeVertUV(float x, float y, float z, float r, float g, float b, float u, float v_) {
    Vertex v;
    v.position = Vec4(x, y, z, 1.0f);
    v.attribs[0] = r; v.attribs[1] = g; v.attribs[2] = b;
    v.attribs[3] = u; v.attribs[4] = v_;
    return v;
}

/* ─── Scene 1: Basic Triangle ──────────────────────────────────────── */
/* EXERCISES: basic pipeline flow, vertex color interpolation.
 * This is the "hello world" of graphics — if this fails, nothing works. */
static void scene_basic_triangle(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::vertexColorFS();
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    std::vector<Vertex> verts = {
        makeVert( 0.0f,  0.8f, 0, 1, 0, 0),
        makeVert(-0.8f, -0.8f, 0, 0, 1, 0),
        makeVert( 0.8f, -0.8f, 0, 0, 0, 1),
    };
    drawTriangles(verts, {}, ps, fb);
}

/* ─── Scene 2: Fill Rule — Shared Edge ─────────────────────────────── */
/* EXERCISES: TOP-LEFT FILL RULE. Two triangles share an edge along the
 * diagonal. Every pixel must be shaded EXACTLY ONCE. This is THE
 * verification test for the fill rule — if any pixel is double-shaded
 * or missed, the rasterizer is WRONG.
 *
 * HARDWARE BEHAVIOR: The rasterizer's fill rule logic (top-left edge
 * classification). Bugs here cause visible seams or overdraw. */
static void scene_fill_rule(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(0, (float)fb.width, (float)fb.height, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Two triangles forming a quad, sharing the diagonal edge.
     * Red triangle: top-left, Blue triangle: bottom-right.
     * The shared edge should be drawn by exactly one triangle. */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> tri1 = {
        makeVert(50, 50, 0, 1, 0, 0),
        makeVert(200, 50, 0, 1, 0, 0),
        makeVert(50, 200, 0, 1, 0, 0),
    };
    drawTriangles(tri1, {}, ps, fb);

    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 0, 255));
    std::vector<Vertex> tri2 = {
        makeVert(200, 50, 0, 0, 0, 1),
        makeVert(200, 200, 0, 0, 0, 1),
        makeVert(50, 200, 0, 0, 0, 1),
    };
    drawTriangles(tri2, {}, ps, fb);
}

/* ─── Scene 3: Degenerate Triangles ────────────────────────────────── */
/* EXERCISES: zero-area and near-zero-area triangles. Hardware must handle
 * these gracefully (no crashes, no artifacts).
 *
 * HARDWARE BEHAVIOR: Edge case handling in triangle setup. Degenerate
 * triangles have zero signed area and must be rejected. */
static void scene_degenerate(Framebuffer& fb) {
    fb.clear(Color(32, 32, 32));
    Mat4 mvp = Mat4::orthoGL(0, (float)fb.width, (float)fb.height, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 255, 0));
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Completely degenerate: all vertices at the same point */
    std::vector<Vertex> degen1 = {
        makeVert(100, 100, 0, 1, 1, 0),
        makeVert(100, 100, 0, 1, 1, 0),
        makeVert(100, 100, 0, 1, 1, 0),
    };
    drawTriangles(degen1, {}, ps, fb);

    /* Collinear vertices: zero area but not coincident */
    std::vector<Vertex> degen2 = {
        makeVert(50, 128, 0, 1, 1, 0),
        makeVert(128, 128, 0, 1, 1, 0),
        makeVert(200, 128, 0, 1, 1, 0),
    };
    drawTriangles(degen2, {}, ps, fb);

    /* Near-degenerate: extremely thin sliver */
    std::vector<Vertex> sliver = {
        makeVert(10, 10, 0, 1, 1, 0),
        makeVert(240, 10.001f, 0, 1, 1, 0),
        makeVert(125, 10.0005f, 0, 1, 1, 0),
    };
    drawTriangles(sliver, {}, ps, fb);

    /* Also draw a normal triangle to prove the pipeline works */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 255, 0));
    std::vector<Vertex> normal = {
        makeVert(80, 150, 0, 0, 1, 0),
        makeVert(180, 150, 0, 0, 1, 0),
        makeVert(130, 230, 0, 0, 1, 0),
    };
    drawTriangles(normal, {}, ps, fb);
}

/* ─── Scene 4: Pixel Center / Edge Cases ───────────────────────────── */
/* EXERCISES: triangles whose edges pass exactly through pixel centers.
 * The fill rule must consistently assign these pixels.
 *
 * HARDWARE BEHAVIOR: The exact handling of the fill rule at pixel centers.
 * This is one of the most common causes of 1-pixel-off differences between
 * GPU vendors. */
static void scene_pixel_centers(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(0, (float)fb.width, (float)fb.height, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Triangle with vertex exactly at pixel center (64.5, 64.5) */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> tri = {
        makeVert(64.5f, 64.5f, 0, 1, 0, 0),
        makeVert(192.5f, 64.5f, 0, 1, 0, 0),
        makeVert(128.5f, 192.5f, 0, 1, 0, 0),
    };
    drawTriangles(tri, {}, ps, fb);

    /* Horizontal edge exactly on pixel row y=128 */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 255, 0));
    std::vector<Vertex> horiz = {
        makeVert(30.0f, 128.5f, 0, 0, 1, 0),
        makeVert(230.0f, 128.5f, 0, 0, 1, 0),
        makeVert(130.0f, 200.5f, 0, 0, 1, 0),
    };
    drawTriangles(horiz, {}, ps, fb);
}

/* ─── Scene 5: Near-Plane Clipping ─────────────────────────────────── */
/* EXERCISES: Sutherland-Hodgman clipping against the near plane. A triangle
 * that partially extends behind the camera.
 *
 * HARDWARE BEHAVIOR: Near-plane clipping in the clipper unit. Without
 * correct clipping, the perspective divide produces nonsense for vertices
 * behind the camera (w < 0 → everything flips). */
static void scene_near_clip(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 view = Mat4::lookAt(Vec3(0, 0, 3), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspectiveGL(1.0f, 1.0f, 1.0f, 100.0f);
    Mat4 mvp = proj * view;

    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::vertexColorFS();
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Triangle with one vertex behind the camera (z=5 in world, camera at z=3) */
    std::vector<Vertex> verts = {
        makeVert(-2.0f, -1.0f,  5.0f, 1, 0, 0),  // behind camera
        makeVert( 2.0f,  0.0f, -2.0f, 0, 1, 0),   // in front
        makeVert( 0.0f,  2.0f, -2.0f, 0, 0, 1),   // in front
    };
    drawTriangles(verts, {}, ps, fb);
}

/* ─── Scene 6: Full Frustum Clipping ───────────────────────────────── */
/* EXERCISES: Clipping against all 6 frustum planes. A huge triangle that
 * extends well beyond the viewport in all directions.
 *
 * HARDWARE BEHAVIOR: Guard band clipping and the full Sutherland-Hodgman
 * clipper. This tests that very large triangles (common in shadow maps
 * and skyboxes) are handled correctly. */
static void scene_frustum_clip(Framebuffer& fb) {
    fb.clear(Color(20, 20, 40));
    Mat4 view = Mat4::lookAt(Vec3(0, 0, 5), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspectiveGL(0.8f, 1.0f, 0.5f, 50.0f);
    Mat4 mvp = proj * view;

    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::vertexColorFS();
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Huge triangle that crosses all frustum planes */
    std::vector<Vertex> verts = {
        makeVert(  0,  50, 0, 1, 0.5f, 0),
        makeVert(-50, -30, 0, 0, 1, 0.5f),
        makeVert( 50, -30, 0, 0.5f, 0, 1),
    };
    drawTriangles(verts, {}, ps, fb);
}

/* ─── Scene 7: Thin Slivers ────────────────────────────────────────── */
/* EXERCISES: Extremely thin triangles. These stress the rasterizer's
 * numerical precision and are common in tessellated geometry.
 *
 * HARDWARE BEHAVIOR: Fixed-point precision in the rasterizer's edge
 * function evaluation. Thin slivers can cause pixels to be missed
 * due to precision limits, especially at high resolutions. */
static void scene_thin_slivers(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(0, (float)fb.width, (float)fb.height, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 255, 0));
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Multiple thin slivers at different angles */
    for (int i = 0; i < 8; ++i) {
        float angle = i * 3.14159f / 8.0f;
        float cx = 128.0f, cy = 128.0f;
        float len = 120.0f;
        float dx = std::cos(angle) * len;
        float dy = std::sin(angle) * len;
        /* width of sliver: 0.3 pixels — very thin */
        float nx = -std::sin(angle) * 0.15f;
        float ny = std::cos(angle) * 0.15f;

        std::vector<Vertex> sliver = {
            makeVert(cx - dx, cy - dy, 0, 1, 1, 0),
            makeVert(cx + dx + nx, cy + dy + ny, 0, 1, 1, 0),
            makeVert(cx + dx - nx, cy + dy - ny, 0, 1, 1, 0),
        };
        drawTriangles(sliver, {}, ps, fb);
    }
}

/* ─── Scene 8: Z-Fighting ──────────────────────────────────────────── */
/* EXERCISES: Coplanar geometry with nearly identical depth values.
 *
 * HARDWARE BEHAVIOR: Depth buffer precision and the depth comparison
 * function. Z-fighting occurs when two surfaces are so close in depth
 * that their per-pixel depth values compete due to precision limits.
 * This is why polygon offset (depth bias) exists in the API. */
static void scene_z_fighting(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Two overlapping triangles at nearly the same depth */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> t1 = {
        makeVert(-0.8f, -0.8f, 0.0f, 1, 0, 0),
        makeVert( 0.8f, -0.8f, 0.0f, 1, 0, 0),
        makeVert( 0.0f,  0.8f, 0.0f, 1, 0, 0),
    };
    drawTriangles(t1, {}, ps, fb);

    /* Second triangle at depth 0.0001 — should win depth test */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 0, 255));
    std::vector<Vertex> t2 = {
        makeVert(-0.6f, -0.6f, -0.0001f, 0, 0, 1),
        makeVert( 0.6f, -0.6f, -0.0001f, 0, 0, 1),
        makeVert( 0.0f,  0.6f, -0.0001f, 0, 0, 1),
    };
    drawTriangles(t2, {}, ps, fb);
}

/* ─── Scene 9: Backface Culling ────────────────────────────────────── */
/* EXERCISES: Front-face and back-face determination via signed area.
 *
 * HARDWARE BEHAVIOR: The face-culling unit after triangle setup.
 * CW winding = back-facing with CCW front-face convention. */
static void scene_backface(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::BACK;  // cull back faces
    ps.frontFace = FrontFace::CCW;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* CCW triangle — should be visible (front-facing) */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 255, 0));
    std::vector<Vertex> ccw = {
        makeVert(-0.8f, -0.5f, 0, 0, 1, 0),
        makeVert(-0.1f, -0.5f, 0, 0, 1, 0),
        makeVert(-0.45f, 0.5f, 0, 0, 1, 0),
    };
    drawTriangles(ccw, {}, ps, fb);

    /* CW triangle — should be CULLED (back-facing) */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> cw = {
        makeVert(0.1f, -0.5f, 0, 1, 0, 0),
        makeVert(0.45f, 0.5f, 0, 1, 0, 0),
        makeVert(0.8f, -0.5f, 0, 1, 0, 0),
    };
    drawTriangles(cw, {}, ps, fb);
}

/* ─── Scene 10: Alpha Blending Order ───────────────────────────────── */
/* EXERCISES: Order-dependent transparency. Overlapping translucent
 * triangles drawn in different orders produce different results.
 *
 * HARDWARE BEHAVIOR: The ROP (Render Output Unit) blend operation.
 * Alpha blending is NOT commutative. This is why order-independent
 * transparency (OIT) is an active research area. */
static void scene_blend_order(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.depthWrite = false;  // translucent objects typically don't write depth
    ps.depthFunc = DepthFunc::ALWAYS;
    ps.blendMode = BlendMode::ALPHA;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Red, then green, then blue — overlapping */
    Color colors[] = {Color(255, 0, 0, 128), Color(0, 255, 0, 128), Color(0, 0, 255, 128)};
    float offsets[] = {-0.2f, 0.0f, 0.2f};

    for (int i = 0; i < 3; ++i) {
        ps.fragmentShader = shaders::makeFlatColorFS(colors[i]);
        float ox = offsets[i];
        std::vector<Vertex> tri = {
            makeVert(-0.5f + ox, -0.5f, 0, 0, 0, 0),
            makeVert( 0.5f + ox, -0.5f, 0, 0, 0, 0),
            makeVert( 0.0f + ox,  0.5f, 0, 0, 0, 0),
        };
        drawTriangles(tri, {}, ps, fb);
    }
}

/* ─── Scene 11: Depth Test Modes ───────────────────────────────────── */
/* EXERCISES: Different depth comparison functions.
 *
 * HARDWARE BEHAVIOR: The depth test unit (part of the ROP). Tests
 * LESS, GREATER, EQUAL, and ALWAYS. */
static void scene_depth_test(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0), 0.5f); // clear depth to 0.5
    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* Triangle at depth 0.3 with LESS test — should pass (0.3 < 0.5) */
    ps.depthFunc = DepthFunc::LESS;
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 255, 0));
    std::vector<Vertex> t1 = {
        makeVert(-0.9f, -0.9f, -0.4f, 0, 1, 0),
        makeVert(-0.1f, -0.9f, -0.4f, 0, 1, 0),
        makeVert(-0.5f,  0.9f, -0.4f, 0, 1, 0),
    };
    drawTriangles(t1, {}, ps, fb);

    /* Triangle at depth 0.7 with LESS test — should FAIL (0.7 > 0.5) */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> t2 = {
        makeVert(0.1f, -0.9f, 0.4f, 1, 0, 0),
        makeVert(0.9f, -0.9f, 0.4f, 1, 0, 0),
        makeVert(0.5f,  0.9f, 0.4f, 1, 0, 0),
    };
    drawTriangles(t2, {}, ps, fb);
}

/* ─── Scene 12: Perspective Correct Interpolation ──────────────────── */
/* EXERCISES: Perspective-correct vs affine interpolation.
 *
 * HARDWARE BEHAVIOR: The attribute interpolation hardware in the fragment
 * shader input stage. Without perspective correction (as on PS1), textures
 * appear to "swim" on surfaces viewed at an angle. */
static void scene_persp_correct(Framebuffer& fb) {
    fb.clear(Color(0, 0, 0));
    Mat4 view = Mat4::lookAt(Vec3(0, 2, 5), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspectiveGL(0.8f, 1.0f, 0.1f, 100.0f);
    Mat4 mvp = proj * view;

    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 5);
    ps.fragmentShader = shaders::makeCheckerFS(8.0f);
    ps.numAttribs = 5;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)fb.width, (float)fb.height);

    /* A floor quad viewed at an angle — perspective correction is visible */
    std::vector<Vertex> verts = {
        makeVertUV(-3, 0, -3, 1, 1, 1, 0, 0),
        makeVertUV( 3, 0, -3, 1, 1, 1, 1, 0),
        makeVertUV( 3, 0,  3, 1, 1, 1, 1, 1),
        makeVertUV(-3, 0, -3, 1, 1, 1, 0, 0),
        makeVertUV( 3, 0,  3, 1, 1, 1, 1, 1),
        makeVertUV(-3, 0,  3, 1, 1, 1, 0, 1),
    };
    drawTriangles(verts, {}, ps, fb);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SCENE REGISTRY
 * ═══════════════════════════════════════════════════════════════════════ */
static const Scene ALL_SCENES[] = {
    {1, "basic_triangle",     "Basic pipeline flow, vertex color interpolation",        scene_basic_triangle},
    {2, "fill_rule",          "TOP-LEFT FILL RULE: shared edge, no double-shade/gaps",  scene_fill_rule},
    {3, "degenerate",         "Zero-area and near-zero-area triangles",                 scene_degenerate},
    {4, "pixel_centers",      "Vertices/edges exactly on pixel centers",                scene_pixel_centers},
    {5, "near_clip",          "Near-plane clipping (vertex behind camera)",              scene_near_clip},
    {6, "frustum_clip",       "Full frustum clipping (huge triangle)",                  scene_frustum_clip},
    {7, "thin_slivers",       "Extremely thin triangles (precision stress)",            scene_thin_slivers},
    {8, "z_fighting",         "Coplanar geometry with near-identical depth",            scene_z_fighting},
    {9, "backface",           "Backface culling (CW vs CCW)",                           scene_backface},
    {10,"blend_order",        "Order-dependent alpha blending",                         scene_blend_order},
    {11,"depth_test",         "Depth comparison functions",                             scene_depth_test},
    {12,"persp_correct",      "Perspective-correct interpolation",                      scene_persp_correct},
};
static const int NUM_SCENES = sizeof(ALL_SCENES) / sizeof(ALL_SCENES[0]);

} // namespace softras

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN — CLI
 * ═══════════════════════════════════════════════════════════════════════ */
int main(int argc, char* argv[]) {
    using namespace softras;

    bool listScenes = false;
    bool hashOnly = false;
    int sceneId = -1;
    std::string outFile;
    bool runAll = false;

    /* Simple argument parsing */
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list-scenes" || arg == "-l") { listScenes = true; }
        else if (arg == "--hash-only") { hashOnly = true; }
        else if (arg == "--all") { runAll = true; }
        else if (arg == "--scene" && i + 1 < argc) { sceneId = std::atoi(argv[++i]); }
        else if (arg == "--out" && i + 1 < argc) { outFile = argv[++i]; }
        else if (arg == "run") { /* ignored — just for "stimulus run --scene N" syntax */ }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    if (listScenes) {
        std::cout << "Available scenes:\n";
        for (int i = 0; i < NUM_SCENES; ++i) {
            std::cout << "  " << ALL_SCENES[i].id << ". " << ALL_SCENES[i].name
                      << "\n     " << ALL_SCENES[i].description << "\n";
        }
        return 0;
    }

    if (runAll) {
        /* Run all scenes and print hashes — useful for regression testing */
        std::cout << "Running all " << NUM_SCENES << " scenes...\n";
        for (int i = 0; i < NUM_SCENES; ++i) {
            Framebuffer fb(FB_W, FB_H);
            ALL_SCENES[i].render(fb);
            std::cout << "  Scene " << std::setw(2) << ALL_SCENES[i].id
                      << " [" << ALL_SCENES[i].name << "]: "
                      << fb.colorHashHex() << "\n";
        }
        return 0;
    }

    if (sceneId < 1 || sceneId > NUM_SCENES) {
        std::cerr << "Usage: stimulus run --scene N [--out img.ppm] [--hash-only]\n"
                  << "       stimulus --list-scenes\n"
                  << "       stimulus --all\n";
        return 1;
    }

    const Scene& scene = ALL_SCENES[sceneId - 1];
    Framebuffer fb(FB_W, FB_H);
    scene.render(fb);

    std::cout << "Scene " << scene.id << " [" << scene.name << "]: "
              << fb.colorHashHex() << "\n";

    if (!outFile.empty() && !hashOnly) {
        /* Detect format from extension */
        if (outFile.size() > 4 && outFile.substr(outFile.size() - 4) == ".bmp")
            fb.writeBMP(outFile);
        else
            fb.writePPM(outFile);
        std::cout << "Wrote: " << outFile << "\n";
    }

    return 0;
}
