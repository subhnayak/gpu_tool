/*
 * texture.h — Texture Sampling with Filtering and Mipmaps
 * =========================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand texture sampling: nearest, bilinear, trilinear, anisotropic
 *   - Understand mipmap generation and LOD selection
 *   - Understand WHY derivatives force quad-based shading (critical hardware detail)
 *
 * INTERVIEW: "Why do GPUs shade fragments in 2x2 quads?"
 * Because texture LOD selection requires screen-space derivatives (ddx/ddy)
 * of the texture coordinates. To compute ddx, you need the texcoord of the
 * fragment to the right; for ddy, the fragment below. These 4 fragments form
 * a 2x2 "quad." Even if some of the 4 fragments are outside the triangle,
 * they must still be shaded (as "helper lanes") to provide their neighbors'
 * derivatives. This wastes work on small triangles (which is why small
 * triangles are bad for GPU performance — many helper lane invocations
 * that compute but discard their results).
 *
 * INTERVIEW: "What is the performance cost of very small triangles?"
 * Each quad has at most 4 fragments. A triangle covering 1 pixel still
 * uses an entire quad (4 fragment shader invocations, 3 of which are
 * helper lanes). Worst case: 75% wasted work. This is why modern GPUs
 * have mesh shaders and variable-rate shading to reduce small-triangle
 * overhead.
 */

#ifndef TEXTURE_H
#define TEXTURE_H

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cassert>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * TEXTURE
 * ═══════════════════════════════════════════════════════════════════════
 * A simple RGBA8 texture with a mip chain. We store each mip level as
 * a separate flat array. Real GPU textures use tiled/swizzled layouts
 * for cache efficiency, but linear is fine for a reference rasterizer.
 *
 * Addressing: texcoords are [0,1] with wrapping. We support:
 *   - WRAP (repeat): frac(u) * width
 *   - CLAMP: clamp(u, 0, 1) * (width-1)
 */
enum class WrapMode { WRAP, CLAMP };

struct TexColor {
    float r, g, b, a;
    TexColor() : r(0), g(0), b(0), a(1) {}
    TexColor(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}

    TexColor operator+(const TexColor& o) const { return {r+o.r, g+o.g, b+o.b, a+o.a}; }
    TexColor operator*(float s) const { return {r*s, g*s, b*s, a*s}; }
};

class Texture {
public:
    /* Each mip level */
    struct MipLevel {
        int width, height;
        std::vector<uint8_t> data; // RGBA, 4 bytes per texel

        TexColor fetch(int x, int y) const {
            assert(x >= 0 && x < width && y >= 0 && y < height);
            int idx = (y * width + x) * 4;
            return {data[idx] / 255.0f, data[idx+1] / 255.0f,
                    data[idx+2] / 255.0f, data[idx+3] / 255.0f};
        }
    };

    std::vector<MipLevel> mips;
    WrapMode wrapU = WrapMode::WRAP;
    WrapMode wrapV = WrapMode::WRAP;

    Texture() = default;

    /* Create a texture from raw RGBA data and generate the mip chain.
     * Width and height should be powers of 2 for clean mipmap generation,
     * but we handle non-power-of-2 as well.
     *
     * INTERVIEW: "How are mipmaps generated?"
     * Each level is half the resolution of the previous. The standard
     * approach is a 2x2 box filter (average 4 texels). Higher-quality
     * mip generation uses Lanczos or Kaiser filters. GPU hardware can
     * generate mips, but game engines often precompute them offline for
     * quality control.
     */
    Texture(int w, int h, const uint8_t* rgba_data) {
        MipLevel level0;
        level0.width = w;
        level0.height = h;
        level0.data.assign(rgba_data, rgba_data + w * h * 4);
        mips.push_back(std::move(level0));
        generateMips();
    }

    /* Generate a procedural checkerboard texture — useful for testing. */
    static Texture checkerboard(int size, int check_size,
                                uint8_t r0 = 255, uint8_t g0 = 255, uint8_t b0 = 255,
                                uint8_t r1 = 0, uint8_t g1 = 0, uint8_t b1 = 0) {
        std::vector<uint8_t> data(size * size * 4);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                bool white = ((x / check_size) + (y / check_size)) % 2 == 0;
                int idx = (y * size + x) * 4;
                data[idx]   = white ? r0 : r1;
                data[idx+1] = white ? g0 : g1;
                data[idx+2] = white ? b0 : b1;
                data[idx+3] = 255;
            }
        return Texture(size, size, data.data());
    }

    /* ═══════════════════════════════════════════════════════════════════
     * MIP CHAIN GENERATION
     * ═══════════════════════════════════════════════════════════════════
     * Downsample each level by 2x using a box filter until 1x1.
     */
    void generateMips() {
        while (mips.back().width > 1 || mips.back().height > 1) {
            const MipLevel& prev = mips.back();
            MipLevel next;
            next.width = std::max(1, prev.width / 2);
            next.height = std::max(1, prev.height / 2);
            next.data.resize(next.width * next.height * 4);

            for (int y = 0; y < next.height; ++y)
                for (int x = 0; x < next.width; ++x) {
                    /* Average the 2x2 block from the previous level */
                    int sx = x * 2, sy = y * 2;
                    int sx1 = std::min(sx + 1, prev.width - 1);
                    int sy1 = std::min(sy + 1, prev.height - 1);

                    auto sample = [&](int px, int py) -> TexColor {
                        return prev.fetch(px, py);
                    };

                    TexColor avg = (sample(sx, sy) + sample(sx1, sy) +
                                    sample(sx, sy1) + sample(sx1, sy1)) * 0.25f;

                    int idx = (y * next.width + x) * 4;
                    next.data[idx]   = static_cast<uint8_t>(avg.r * 255.0f + 0.5f);
                    next.data[idx+1] = static_cast<uint8_t>(avg.g * 255.0f + 0.5f);
                    next.data[idx+2] = static_cast<uint8_t>(avg.b * 255.0f + 0.5f);
                    next.data[idx+3] = static_cast<uint8_t>(avg.a * 255.0f + 0.5f);
                }
            mips.push_back(std::move(next));
        }
    }

    /* ═══════════════════════════════════════════════════════════════════
     * LOD SELECTION FROM DERIVATIVES
     * ═══════════════════════════════════════════════════════════════════
     * The LOD (Level of Detail) determines which mip level to sample.
     * It's computed from the screen-space derivatives of the texture
     * coordinates:
     *
     *   LOD = log2(max(|du/dx|, |du/dy|, |dv/dx|, |dv/dy|) * texture_size)
     *
     * More precisely, we want the pixel-to-texel ratio. If one screen
     * pixel covers 4 texels, we want LOD = log2(4) = 2.
     *
     * INTERVIEW: "How does the GPU compute texture LOD?"
     * The fragment shader runs in 2x2 quads. For each quad, the hardware
     * computes finite differences:
     *   ddx(u) = u(x+1,y) - u(x,y)    // difference from right neighbor
     *   ddy(u) = u(x,y+1) - u(x,y)    // difference from bottom neighbor
     * Then LOD = log2(max(length(ddx(u,v)), length(ddy(u,v)))) approximately.
     *
     * Parameters:
     *   dudx, dvdx: derivative of texcoord with respect to screen x
     *   dudy, dvdy: derivative of texcoord with respect to screen y
     */
    float computeLOD(float dudx, float dvdx, float dudy, float dvdy, int level0_w, int level0_h) const {
        /* Scale derivatives by texture dimensions to get texel-space derivatives */
        float ddx_len = std::sqrt((dudx * level0_w) * (dudx * level0_w) +
                                  (dvdx * level0_h) * (dvdx * level0_h));
        float ddy_len = std::sqrt((dudy * level0_w) * (dudy * level0_w) +
                                  (dvdy * level0_h) * (dvdy * level0_h));

        /* Use the larger derivative — this is the "isotropic" approximation.
         * Anisotropic filtering would consider the ratio between the two. */
        float rho = std::max(ddx_len, ddy_len);
        if (rho <= 0.0f) return 0.0f;
        return std::log2(rho);
    }

    /* ═══════════════════════════════════════════════════════════════════
     * TEXTURE SAMPLING
     * ═══════════════════════════════════════════════════════════════════ */

    /* Apply wrap mode to a texcoord component. */
    float applyWrap(float t, WrapMode mode) const {
        if (mode == WrapMode::WRAP) {
            t = t - std::floor(t); // frac — wraps to [0, 1)
        } else {
            t = std::max(0.0f, std::min(1.0f, t));
        }
        return t;
    }

    /* NEAREST FILTERING (point sampling):
     * Pick the single closest texel. Fast but produces visible "blockiness"
     * when the texture is magnified, and aliasing (moiré patterns) when
     * minified. Real hardware does this with a simple truncation.
     */
    TexColor sampleNearest(float u, float v, int mipLevel = 0) const {
        mipLevel = std::max(0, std::min(mipLevel, (int)mips.size() - 1));
        const MipLevel& mip = mips[mipLevel];

        u = applyWrap(u, wrapU);
        v = applyWrap(v, wrapV);

        int x = std::min((int)(u * mip.width), mip.width - 1);
        int y = std::min((int)(v * mip.height), mip.height - 1);
        return mip.fetch(x, y);
    }

    /* BILINEAR FILTERING:
     * Interpolate between the 4 nearest texels. Removes "blockiness" on
     * magnification, but still aliases on minification (mipmaps help).
     *
     * Real GPU hardware has dedicated texture units that do this in a
     * single clock cycle — it's one of the most optimized operations
     * in the entire GPU pipeline. A modern GPU can do billions of
     * bilinear-filtered texture fetches per second.
     *
     * INTERVIEW: "What is bilinear filtering?"
     * Bilinear = two linear interpolations. Sample 4 texels in a 2x2 grid,
     * lerp horizontally (2 lerps), then lerp the results vertically (1 lerp).
     * Result: smooth transition between texels.
     */
    TexColor sampleBilinear(float u, float v, int mipLevel = 0) const {
        mipLevel = std::max(0, std::min(mipLevel, (int)mips.size() - 1));
        const MipLevel& mip = mips[mipLevel];

        u = applyWrap(u, wrapU);
        v = applyWrap(v, wrapV);

        float fx = u * mip.width - 0.5f;
        float fy = v * mip.height - 0.5f;

        int x0 = (int)std::floor(fx);
        int y0 = (int)std::floor(fy);
        float sx = fx - x0; // fractional part (blend weight)
        float sy = fy - y0;

        /* Wrap texel coordinates */
        auto wrap_coord = [](int c, int size, WrapMode mode) -> int {
            if (mode == WrapMode::WRAP) {
                c = c % size;
                if (c < 0) c += size;
                return c;
            }
            return std::max(0, std::min(c, size - 1));
        };

        int x1 = wrap_coord(x0 + 1, mip.width, wrapU);
        int y1 = wrap_coord(y0 + 1, mip.height, wrapV);
        x0 = wrap_coord(x0, mip.width, wrapU);
        y0 = wrap_coord(y0, mip.height, wrapV);

        /* Fetch 4 texels and bilinear blend */
        TexColor c00 = mip.fetch(x0, y0);
        TexColor c10 = mip.fetch(x1, y0);
        TexColor c01 = mip.fetch(x0, y1);
        TexColor c11 = mip.fetch(x1, y1);

        /* Lerp: result = (1-s)*(1-t)*c00 + s*(1-t)*c10 + (1-s)*t*c01 + s*t*c11 */
        TexColor top = c00 * (1.0f - sx) + c10 * sx;
        TexColor bot = c01 * (1.0f - sx) + c11 * sx;
        return top * (1.0f - sy) + bot * sy;
    }

    /* Sample with automatic LOD selection using derivatives.
     * This is what the GPU does for every textured fragment. */
    TexColor sampleWithDerivatives(float u, float v,
                                   float dudx, float dvdx,
                                   float dudy, float dvdy,
                                   bool bilinear = true) const {
        if (mips.empty()) return {};
        float lod = computeLOD(dudx, dvdx, dudy, dvdy, mips[0].width, mips[0].height);
        int mipLevel = std::max(0, std::min((int)(lod + 0.5f), (int)mips.size() - 1));
        return bilinear ? sampleBilinear(u, v, mipLevel)
                        : sampleNearest(u, v, mipLevel);
    }

    int mipCount() const { return (int)mips.size(); }
};

} // namespace softras

#endif // TEXTURE_H
