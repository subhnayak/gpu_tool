/*
 * math3d.h — Minimal 3D Math Library for Software Rasterizer
 * ===========================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand the full vertex transform chain: Model → View → Projection → Clip → NDC → Viewport
 *   - Know the difference between GL-style and D3D/Vulkan-style projection conventions
 *   - Be able to derive projection matrices on a whiteboard (CLASSIC interview question)
 *
 * INTERVIEW QUESTION: "Walk me through the coordinate spaces a vertex goes through
 * from object space to pixels on screen."
 * Answer: Object → (Model matrix) → World → (View matrix) → Eye/Camera →
 *         (Projection matrix) → Clip space → (Perspective divide) → NDC →
 *         (Viewport transform) → Screen/Window coordinates
 *
 * CONVENTION DIFFERENCES (know these for interviews!):
 *   - OpenGL: NDC depth [-1, +1], right-handed, Y-up in framebuffer, column-major matrices
 *   - D3D/Vulkan: NDC depth [0, 1], left-handed in clip space (D3D) / right-handed (Vulkan),
 *     Y-down in framebuffer (Vulkan), row-major matrices (D3D)
 *   - Metal: NDC depth [0, 1], left-handed clip, Y-up in framebuffer
 *   - These differences cause bugs when porting and are heavily tested in GPU verification
 *
 * NOTE ON DETERMINISM: All operations here are IEEE 754 compliant single-precision.
 * We avoid reassociation and fast-math. See DETERMINISM.md for why this matters.
 */

#ifndef MATH3D_H
#define MATH3D_H

#include <cmath>
#include <cstring>
#include <cassert>
#include <algorithm>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * VECTOR TYPES
 * ═══════════════════════════════════════════════════════════════════════
 * These are plain data types with operations. Real GPU hardware processes
 * these in SIMD lanes — each component of a vec4 maps to one lane in a
 * 128-bit SIMD unit (SSE/NEON). A "core" on a GPU typically has 32 such
 * SIMD lanes processing 32 vertices/fragments in lockstep (a "warp").
 */

struct Vec2 {
    float x, y;
    Vec2() : x(0), y(0) {}
    Vec2(float x, float y) : x(x), y(y) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    float dot(const Vec2& o) const { return x * o.x + y * o.y; }
};

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { float inv = 1.0f / s; return {x * inv, y * inv, z * inv}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

    /* Cross product: fundamental for normals, backface culling, and the
     * edge function used in rasterization. On real GPUs, this is typically
     * done with a sequence of MUL + MAD instructions. */
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y,
                z * o.x - x * o.z,
                x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const { return *this / length(); }
};

struct Vec4 {
    float x, y, z, w;
    Vec4() : x(0), y(0), z(0), w(0) {}
    Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    Vec4(const Vec3& v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    float dot(const Vec4& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }

    /* Perspective divide: clip space → NDC. This is where w becomes "useful":
     * after projection, w holds the original eye-space -z (for GL-style).
     * Dividing by w produces normalized device coordinates.
     * INTERVIEW: "What happens if w is zero?" → The vertex is on the eye plane,
     * which is degenerate. Clipping must remove such vertices before we get here. */
    Vec3 perspDivide() const {
        assert(w != 0.0f && "Perspective divide by zero — vertex on eye plane. Clip first!");
        float inv_w = 1.0f / w;
        return {x * inv_w, y * inv_w, z * inv_w};
    }

    Vec3 xyz() const { return {x, y, z}; }
};

/* ═══════════════════════════════════════════════════════════════════════
 * 4x4 MATRIX
 * ═══════════════════════════════════════════════════════════════════════
 * We store in COLUMN-MAJOR order (like OpenGL/GLSL):
 *   m[col][row], so m[0] is the first column.
 *
 * INTERVIEW: "Why column-major?" — GLSL uses column-major because
 * matrix-vector multiply (M * v) naturally reads columns: result[row] =
 * dot(row_of_M, v), and reading a column from column-major storage is
 * a contiguous memory access. Row-major (HLSL/D3D) makes v * M efficient.
 * GPUs don't care much (they have register files), but the convention
 * affects how you upload uniforms via the API.
 */
struct Mat4 {
    float m[4][4]; // m[col][row]

    Mat4() { std::memset(m, 0, sizeof(m)); }

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    /* Matrix-vector multiply: transforms a point/direction through the matrix.
     * This is the single most common operation in vertex processing.
     * On real GPU hardware, this is 4 dot-products, each typically 4 MAD
     * instructions = 16 MADs total per vertex transform. */
    Vec4 operator*(const Vec4& v) const {
        return {
            m[0][0]*v.x + m[1][0]*v.y + m[2][0]*v.z + m[3][0]*v.w,
            m[0][1]*v.x + m[1][1]*v.y + m[2][1]*v.z + m[3][1]*v.w,
            m[0][2]*v.x + m[1][2]*v.y + m[2][2]*v.z + m[3][2]*v.w,
            m[0][3]*v.x + m[1][3]*v.y + m[2][3]*v.z + m[3][3]*v.w
        };
    }

    /* Matrix-matrix multiply: compose transforms. Order matters!
     * M = Projection * View * Model means: first apply Model, then View, then Projection.
     * This is because we evaluate M * v = (P * (V * (Mo * v))). */
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += m[k][row] * o.m[c][k];
                r.m[c][row] = sum;
            }
        return r;
    }

    /* ═══════════════════════════════════════════════════════════════════
     * TRANSFORM BUILDERS
     * ═══════════════════════════════════════════════════════════════════ */

    /* Translation: moves objects in world space. The translation vector
     * goes in the last column (column-major). */
    static Mat4 translate(float tx, float ty, float tz) {
        Mat4 r = identity();
        r.m[3][0] = tx; r.m[3][1] = ty; r.m[3][2] = tz;
        return r;
    }

    /* Scale: non-uniform scaling along axes. Negative scale flips
     * the winding order of triangles (important for culling!). */
    static Mat4 scale(float sx, float sy, float sz) {
        Mat4 r = identity();
        r.m[0][0] = sx; r.m[1][1] = sy; r.m[2][2] = sz;
        return r;
    }

    /* Rotation about X, Y, Z axes. Real engines use quaternions to avoid
     * gimbal lock, but axis-angle rotations are clearer for teaching. */
    static Mat4 rotateX(float radians) {
        Mat4 r = identity();
        float c = std::cos(radians), s = std::sin(radians);
        r.m[1][1] = c;  r.m[2][1] = -s;
        r.m[1][2] = s;  r.m[2][2] = c;
        return r;
    }
    static Mat4 rotateY(float radians) {
        Mat4 r = identity();
        float c = std::cos(radians), s = std::sin(radians);
        r.m[0][0] = c;  r.m[2][0] = s;
        r.m[0][2] = -s; r.m[2][2] = c;
        return r;
    }
    static Mat4 rotateZ(float radians) {
        Mat4 r = identity();
        float c = std::cos(radians), s = std::sin(radians);
        r.m[0][0] = c;  r.m[1][0] = -s;
        r.m[0][1] = s;  r.m[1][1] = c;
        return r;
    }

    /* ═══════════════════════════════════════════════════════════════════
     * VIEW MATRIX (lookAt)
     * ═══════════════════════════════════════════════════════════════════
     * Transforms from world space to camera/eye space.
     * The camera is at 'eye', looking toward 'center', with 'up' defining
     * the vertical.
     *
     * INTERVIEW: "Derive the lookAt matrix."
     * 1. Compute the camera basis vectors: forward (f), right (r), up (u)
     * 2. Build a rotation that aligns them with -Z, +X, +Y
     * 3. Prepend translation by -eye
     * Result: View = [r.x  r.y  r.z  -r·eye]
     *                [u.x  u.y  u.z  -u·eye]
     *                [-f.x -f.y -f.z  f·eye]   ← looking down -Z in GL
     *                [0    0    0    1      ]
     */
    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = (center - eye).normalized();  // forward
        Vec3 r = f.cross(up).normalized();     // right
        Vec3 u = r.cross(f);                   // recomputed up (orthogonal)

        Mat4 mat = identity();
        mat.m[0][0] = r.x;  mat.m[1][0] = r.y;  mat.m[2][0] = r.z;
        mat.m[0][1] = u.x;  mat.m[1][1] = u.y;  mat.m[2][1] = u.z;
        mat.m[0][2] = -f.x; mat.m[1][2] = -f.y; mat.m[2][2] = -f.z;
        mat.m[3][0] = -(r.dot(eye));
        mat.m[3][1] = -(u.dot(eye));
        mat.m[3][2] = f.dot(eye);
        return mat;
    }

    /* ═══════════════════════════════════════════════════════════════════
     * PERSPECTIVE PROJECTION — GL STYLE (depth range [-1, +1])
     * ═══════════════════════════════════════════════════════════════════
     *
     * Maps the view frustum to a [-1,+1]^3 cube (OpenGL NDC).
     *
     * The matrix:
     *   [2n/(r-l)  0        (r+l)/(r-l)   0          ]
     *   [0         2n/(t-b) (t+b)/(t-b)   0          ]
     *   [0         0        -(f+n)/(f-n)  -2fn/(f-n)  ]
     *   [0         0        -1             0          ]
     *
     * For a symmetric frustum (the common case with fov):
     *   l = -r, b = -t, so (r+l)=0, (t+b)=0.
     *
     * INTERVIEW: "Why does the projection matrix put -z into w?"
     * Because in eye space, objects in front of the camera have negative z
     * (camera looks down -Z in GL convention). We need w > 0 for correct
     * perspective divide, so we negate z: w_clip = -z_eye.
     */
    static Mat4 perspectiveGL(float fovY_rad, float aspect, float near, float far) {
        float t = near * std::tan(fovY_rad * 0.5f);
        float r = t * aspect;

        Mat4 mat;
        mat.m[0][0] = near / r;
        mat.m[1][1] = near / t;
        mat.m[2][2] = -(far + near) / (far - near);
        mat.m[2][3] = -1.0f;
        mat.m[3][2] = -2.0f * far * near / (far - near);
        return mat;
    }

    /* ═══════════════════════════════════════════════════════════════════
     * PERSPECTIVE PROJECTION — D3D/VULKAN STYLE (depth range [0, 1])
     * ═══════════════════════════════════════════════════════════════════
     *
     * Maps the view frustum so depth goes from 0 (near) to 1 (far).
     *
     * The matrix (symmetric frustum):
     *   [1/(a*tan(fov/2))  0              0              0       ]
     *   [0                 1/tan(fov/2)   0              0       ]
     *   [0                 0              f/(n-f)        nf/(n-f)]
     *   [0                 0              -1             0       ]
     *
     * KEY DIFFERENCE FROM GL:
     *   - GL: z_ndc ∈ [-1, +1]. Half the depth precision is wasted behind the camera.
     *   - D3D/VK: z_ndc ∈ [0, 1]. Full precision used.
     *   - This is why Vulkan adopted [0,1] — better depth precision.
     *   - See also: reversed-Z (EXERCISES.md) which puts near at 1 and far at 0
     *     to exploit floating-point's higher precision near zero.
     *
     * INTERVIEW: "Why does [0,1] depth have better precision than [-1,1]?"
     * Because IEEE 754 floats have more precision near zero. With [-1,1],
     * the near plane maps to -1, wasting half the float range. With [0,1]
     * and reversed-Z, the near plane maps to 1.0 (exact) and far maps to
     * 0.0 (exact), and the high-precision small values cover the far range
     * where you need them most.
     */
    static Mat4 perspectiveVK(float fovY_rad, float aspect, float near, float far) {
        float t = near * std::tan(fovY_rad * 0.5f);
        float r = t * aspect;

        Mat4 mat;
        mat.m[0][0] = near / r;
        mat.m[1][1] = near / t;
        mat.m[2][2] = far / (near - far);
        mat.m[2][3] = -1.0f;
        mat.m[3][2] = (near * far) / (near - far);
        return mat;
    }

    /* ═══════════════════════════════════════════════════════════════════
     * ORTHOGRAPHIC PROJECTION — GL STYLE
     * ═══════════════════════════════════════════════════════════════════
     *
     * No perspective — parallel lines stay parallel. Used for UI, 2D
     * games, shadow maps, and CAD.
     *
     * Maps the box [l,r] × [b,t] × [n,f] to [-1,+1]^3.
     */
    static Mat4 orthoGL(float l, float r, float b, float t, float n, float f) {
        Mat4 mat;
        mat.m[0][0] = 2.0f / (r - l);
        mat.m[1][1] = 2.0f / (t - b);
        mat.m[2][2] = -2.0f / (f - n);
        mat.m[3][0] = -(r + l) / (r - l);
        mat.m[3][1] = -(t + b) / (t - b);
        mat.m[3][2] = -(f + n) / (f - n);
        mat.m[3][3] = 1.0f;
        return mat;
    }

    /* Orthographic — D3D/Vulkan style (depth [0, 1]). */
    static Mat4 orthoVK(float l, float r, float b, float t, float n, float f) {
        Mat4 mat;
        mat.m[0][0] = 2.0f / (r - l);
        mat.m[1][1] = 2.0f / (t - b);
        mat.m[2][2] = -1.0f / (f - n);
        mat.m[3][0] = -(r + l) / (r - l);
        mat.m[3][1] = -(t + b) / (t - b);
        mat.m[3][2] = -n / (f - n);
        mat.m[3][3] = 1.0f;
        return mat;
    }
};

/* ═══════════════════════════════════════════════════════════════════════
 * VIEWPORT TRANSFORM
 * ═══════════════════════════════════════════════════════════════════════
 * Converts NDC coordinates to screen-space pixel coordinates.
 *
 * GL convention:  x_screen = (x_ndc + 1) * width/2 + x_offset
 *                 y_screen = (y_ndc + 1) * height/2 + y_offset
 *
 * Note: In GL, (0,0) is bottom-left. In D3D/Vulkan framebuffers,
 * (0,0) is top-left. Vulkan flips Y in the viewport (negative height)
 * or in the projection matrix. This is a common source of "upside-down"
 * bugs when porting between APIs.
 *
 * INTERVIEW: "My rendering is upside down in Vulkan — why?"
 * Vulkan's framebuffer Y is top-down, opposite to GL. Fix by negating
 * the viewport height or flipping Y in the projection matrix.
 *
 * For our software rasterizer: (0,0) = top-left, Y increases downward.
 * This matches image conventions and simplifies buffer layout.
 */
struct Viewport {
    float x, y;        // top-left corner offset
    float width, height;
    float minDepth, maxDepth; // depth range: [0,1] for D3D/VK, [-1,1]→[0,1] for GL

    Viewport() : x(0), y(0), width(640), height(480), minDepth(0.0f), maxDepth(1.0f) {}
    Viewport(float w, float h) : x(0), y(0), width(w), height(h), minDepth(0.0f), maxDepth(1.0f) {}

    /* Transform NDC to screen coordinates.
     * Input: NDC (after perspective divide), where x,y ∈ [-1,1], z ∈ [-1,1] (GL) or [0,1] (VK)
     * Output: screen x,y in pixels, z mapped to [minDepth, maxDepth]
     *
     * We use the GL→screen convention here:
     *   screen_x = (ndc.x + 1) * 0.5 * width + x_offset
     *   screen_y = (1 - ndc.y) * 0.5 * height + y_offset    ← Y flip for top-left origin
     *   screen_z = (ndc.z + 1) * 0.5 * (maxDepth - minDepth) + minDepth   (for GL NDC)
     */
    Vec3 transform(const Vec3& ndc) const {
        return {
            (ndc.x + 1.0f) * 0.5f * width + x,
            (1.0f - ndc.y) * 0.5f * height + y,   // flip Y: NDC Y-up → screen Y-down
            (ndc.z + 1.0f) * 0.5f * (maxDepth - minDepth) + minDepth
        };
    }
};

} // namespace softras

#endif // MATH3D_H
