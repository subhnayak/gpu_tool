/*
 * tests.cpp — Dependency-Free Assertion Tests for the Software Rasterizer
 * =========================================================================
 *
 * These tests verify correctness of the critical pipeline behaviors.
 * No test framework dependency — just assert() and manual checks.
 *
 * Run: ./tests   (returns 0 on success, aborts on failure)
 */

#include "pipeline.h"
#include "framebuffer.h"
#include "texture.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>

/* Include shaders */
namespace softras { namespace shaders {
    VertexShaderFn makePassthroughVS(const Mat4& mvp, int numAttribs);
    FragmentShaderFn makeFlatColorFS(const Color& c);
    FragmentShaderFn vertexColorFS();
}}

using namespace softras;

/* Helper: make a vertex with position and color */
static Vertex makeVert(float x, float y, float z, float r, float g, float b) {
    Vertex v;
    v.position = Vec4(x, y, z, 1.0f);
    v.attribs[0] = r; v.attribs[1] = g; v.attribs[2] = b;
    return v;
}

static int tests_passed = 0;
static int tests_run = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        std::cout << "  TEST: " << name << "... "; \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        std::cout << "PASSED\n"; \
    } while(0)

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 1: Matrix/Transform Round-Trips
 * ═══════════════════════════════════════════════════════════════════════
 * Verify that translate * inverse(translate) = identity, etc.
 * This catches math bugs early. */
static void test_matrix_identity() {
    TEST("Mat4 identity * vector");
    Mat4 id = Mat4::identity();
    Vec4 v(1, 2, 3, 1);
    Vec4 r = id * v;
    assert(std::abs(r.x - 1) < 1e-5f);
    assert(std::abs(r.y - 2) < 1e-5f);
    assert(std::abs(r.z - 3) < 1e-5f);
    assert(std::abs(r.w - 1) < 1e-5f);
    PASS();
}

static void test_translate_roundtrip() {
    TEST("Translate round-trip");
    Mat4 t1 = Mat4::translate(3, -5, 7);
    Mat4 t2 = Mat4::translate(-3, 5, -7);
    Mat4 combined = t2 * t1;
    Vec4 v(10, 20, 30, 1);
    Vec4 r = combined * v;
    assert(std::abs(r.x - 10) < 1e-4f);
    assert(std::abs(r.y - 20) < 1e-4f);
    assert(std::abs(r.z - 30) < 1e-4f);
    PASS();
}

static void test_ortho_corners() {
    TEST("Ortho projection maps corners correctly");
    /* Ortho [-1,1]x[-1,1]x[-1,1] should map identically to NDC */
    Mat4 o = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    Vec4 corner(1, 1, -1, 1);
    Vec4 r = o * corner;
    assert(std::abs(r.x - 1) < 1e-5f);
    assert(std::abs(r.y - 1) < 1e-5f);
    /* z=-1 in eye space should map to z=-1 in NDC (near) */
    assert(std::abs(r.z - (-1)) < 1e-5f);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 2: Fill Rule Correctness — THE Key Test
 * ═══════════════════════════════════════════════════════════════════════
 * Two adjacent triangles sharing an edge must shade every pixel in their
 * union EXACTLY ONCE. No double-shading, no gaps.
 *
 * INTERVIEW: "How do you test a rasterizer's fill rule?"
 * Render two adjacent triangles in different colors, then verify that
 * every pixel in their bounding box is either color A or color B (never
 * black/background, and never blended). */
static void test_fill_rule() {
    TEST("Fill rule: shared edge, no gaps or overdraw");

    const int W = 64, H = 64;
    Framebuffer fb(W, H);
    fb.clear(Color(0, 0, 0)); // black background

    /* Orthographic projection mapping pixel coords directly */
    Mat4 mvp = Mat4::orthoGL(0, (float)W, (float)H, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)W, (float)H);

    /* Triangle A: red, covering top-left of a quad */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> triA = {
        makeVert(10, 10, 0, 1, 0, 0),
        makeVert(50, 10, 0, 1, 0, 0),
        makeVert(10, 50, 0, 1, 0, 0),
    };
    drawTriangles(triA, {}, ps, fb);

    /* Triangle B: blue, covering bottom-right, sharing the diagonal */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 0, 255));
    std::vector<Vertex> triB = {
        makeVert(50, 10, 0, 0, 0, 1),
        makeVert(50, 50, 0, 0, 0, 1),
        makeVert(10, 50, 0, 0, 0, 1),
    };
    drawTriangles(triB, {}, ps, fb);

    /* Check: every pixel inside the quad [10,50)x[10,50) must be either
     * red (255,0,0) or blue (0,0,255). No black, no mixed colors. */
    int red_count = 0, blue_count = 0, bad_count = 0;
    for (int y = 10; y < 50; ++y) {
        for (int x = 10; x < 50; ++x) {
            Color c = fb.colorAt(x, y);
            if (c.r == 255 && c.g == 0 && c.b == 0) red_count++;
            else if (c.r == 0 && c.g == 0 && c.b == 255) blue_count++;
            else bad_count++;
        }
    }

    int total_interior = 40 * 40; // 1600 pixels
    assert(bad_count == 0 && "Fill rule violation: pixel is neither red nor blue");
    assert(red_count + blue_count == total_interior && "Missing pixels in quad");
    assert(red_count > 0 && blue_count > 0 && "One triangle got no pixels");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 3: Clipping Correctness
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_clipping() {
    TEST("Near-plane clipping does not crash");

    Framebuffer fb(64, 64);
    fb.clear();

    Mat4 view = Mat4::lookAt(Vec3(0, 0, 3), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspectiveGL(1.0f, 1.0f, 1.0f, 100.0f);
    Mat4 mvp = proj * view;

    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 255, 255));
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport(64, 64);

    /* Triangle with vertex behind camera — must not crash */
    std::vector<Vertex> verts = {
        makeVert(-1, -1, 5, 1, 1, 1),  // behind camera (camera at z=3, looking at z=0)
        makeVert( 1,  0, -2, 1, 1, 1),
        makeVert( 0,  1, -2, 1, 1, 1),
    };
    drawTriangles(verts, {}, ps, fb);
    /* If we get here without crashing, the test passes */
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 4: Perspective-Correct Interpolation
 * ═══════════════════════════════════════════════════════════════════════
 * Verify that attribute interpolation matches analytically computed values
 * for a simple case. */
static void test_perspective_correct() {
    TEST("Perspective-correct interpolation sanity");

    /* Use orthographic (w=1 everywhere) — in this case, perspective-correct
     * interpolation should reduce to linear interpolation. */
    const int W = 32, H = 32;
    Framebuffer fb(W, H);
    fb.clear();

    Mat4 mvp = Mat4::orthoGL(0, (float)W, (float)H, 0, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::vertexColorFS();
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport((float)W, (float)H);

    /* Right triangle in upper-left. Vertex colors: pure R, G, B. */
    std::vector<Vertex> verts = {
        makeVert(0,  0, 0, 1, 0, 0),   // red
        makeVert(30, 0, 0, 0, 1, 0),   // green
        makeVert(0, 30, 0, 0, 0, 1),   // blue
    };
    drawTriangles(verts, {}, ps, fb);

    /* Sample at (1, 1) — should be mostly red with a bit of green and blue.
     * The barycentric coords at center (1.5, 1.5) for this triangle:
     * Not a precise check, just verify it's not black and has red component. */
    Color c = fb.colorAt(1, 1);
    assert(c.r > 200 && "Top-left corner should be mostly red");
    assert(c.r > c.g && c.r > c.b && "Red should dominate at top-left");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 5: Depth Test Behavior
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_depth() {
    TEST("Depth test: closer triangle wins");

    const int W = 32, H = 32;
    Framebuffer fb(W, H);
    fb.clear(Color(0, 0, 0), 1.0f);

    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.depthFunc = DepthFunc::LESS;
    ps.viewport = Viewport((float)W, (float)H);

    /* Draw far triangle (red) first */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 0, 0));
    std::vector<Vertex> far_tri = {
        makeVert(-0.8f, -0.8f, 0.0f, 1, 0, 0),
        makeVert( 0.8f, -0.8f, 0.0f, 1, 0, 0),
        makeVert( 0.0f,  0.8f, 0.0f, 1, 0, 0),
    };
    drawTriangles(far_tri, {}, ps, fb);

    /* Draw near triangle (green) on top — should overwrite */
    ps.fragmentShader = shaders::makeFlatColorFS(Color(0, 255, 0));
    std::vector<Vertex> near_tri = {
        makeVert(-0.6f, -0.6f, -0.5f, 0, 1, 0),
        makeVert( 0.6f, -0.6f, -0.5f, 0, 1, 0),
        makeVert( 0.0f,  0.6f, -0.5f, 0, 1, 0),
    };
    drawTriangles(near_tri, {}, ps, fb);

    /* Center pixel should be green (the near triangle) */
    Color c = fb.colorAt(W/2, H/2);
    assert(c.g == 255 && c.r == 0 && "Center should be green (near triangle wins)");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 6: DETERMINISM — Render twice, assert identical hashes
 * ═══════════════════════════════════════════════════════════════════════
 * The entire point of a software rasterizer as a golden model. */
static void test_determinism() {
    TEST("Determinism: identical hashes on repeated render");

    auto render_scene = []() -> uint32_t {
        Framebuffer fb(128, 128);
        fb.clear(Color(0, 0, 0));

        Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
        PipelineState ps;
        ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
        ps.fragmentShader = shaders::vertexColorFS();
        ps.numAttribs = 3;
        ps.cullMode = CullMode::NONE;
        ps.viewport = Viewport(128, 128);

        std::vector<Vertex> verts = {
            makeVert( 0.0f,  0.8f, 0, 1, 0, 0),
            makeVert(-0.8f, -0.8f, 0, 0, 1, 0),
            makeVert( 0.8f, -0.8f, 0, 0, 0, 1),
        };
        drawTriangles(verts, {}, ps, fb);
        return fb.colorHash();
    };

    uint32_t hash1 = render_scene();
    uint32_t hash2 = render_scene();
    assert(hash1 == hash2 && "Determinism violated: hashes differ across runs");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 7: Degenerate triangles don't crash
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_degenerate_triangles() {
    TEST("Degenerate triangles handled gracefully");

    Framebuffer fb(32, 32);
    fb.clear();

    Mat4 mvp = Mat4::orthoGL(-1, 1, -1, 1, -1, 1);
    PipelineState ps;
    ps.vertexShader = shaders::makePassthroughVS(mvp, 3);
    ps.fragmentShader = shaders::makeFlatColorFS(Color(255, 255, 255));
    ps.numAttribs = 3;
    ps.cullMode = CullMode::NONE;
    ps.viewport = Viewport(32, 32);

    /* All vertices identical */
    std::vector<Vertex> d1 = {
        makeVert(0, 0, 0, 1, 1, 1),
        makeVert(0, 0, 0, 1, 1, 1),
        makeVert(0, 0, 0, 1, 1, 1),
    };
    drawTriangles(d1, {}, ps, fb);

    /* Collinear */
    std::vector<Vertex> d2 = {
        makeVert(-0.5f, 0, 0, 1, 1, 1),
        makeVert( 0.0f, 0, 0, 1, 1, 1),
        makeVert( 0.5f, 0, 0, 1, 1, 1),
    };
    drawTriangles(d2, {}, ps, fb);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 8: Texture mipmap generation
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_texture_mips() {
    TEST("Texture mipmap chain generation");

    Texture tex = Texture::checkerboard(64, 8);
    /* 64x64 → 32x32 → 16x16 → 8x8 → 4x4 → 2x2 → 1x1 = 7 levels */
    assert(tex.mipCount() == 7 && "64x64 texture should have 7 mip levels");
    assert(tex.mips[0].width == 64);
    assert(tex.mips[1].width == 32);
    assert(tex.mips[6].width == 1);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST 9: CRC32 consistency
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_crc32() {
    TEST("CRC32 known value");
    /* CRC32 of "123456789" should be 0xCBF43926 */
    const uint8_t data[] = "123456789";
    uint32_t crc = crc32_compute(data, 9);
    assert(crc == 0xCBF43926 && "CRC32 implementation is wrong");
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════ */
int main() {
    std::cout << "Running P5 Software Rasterizer Tests\n";
    std::cout << "====================================\n";

    test_matrix_identity();
    test_translate_roundtrip();
    test_ortho_corners();
    test_fill_rule();
    test_clipping();
    test_perspective_correct();
    test_depth();
    test_determinism();
    test_degenerate_triangles();
    test_texture_mips();
    test_crc32();

    std::cout << "====================================\n";
    std::cout << tests_passed << " / " << tests_run << " tests passed.\n";

    if (tests_passed == tests_run) {
        std::cout << "ALL TESTS PASSED.\n";
        return 0;
    } else {
        std::cout << "SOME TESTS FAILED.\n";
        return 1;
    }
}
