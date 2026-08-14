/*
 * golden_check.cpp — Golden Image Comparison for Verification
 * =============================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand exact vs tolerance-based image comparison
 *   - Know what floating-point invariance guarantees graphics APIs provide
 *   - Understand how GPU verification teams use golden images
 *
 * WHEN TO USE EXACT COMPARISON:
 *   - Software rasterizer with fixed compile flags: ALWAYS exact
 *   - Same GPU, same driver, same shader: usually exact
 *   - Deterministic rendering modes (GL_INVARIANT_BIT, etc.)
 *
 * WHEN TO USE TOLERANCE:
 *   - Cross-vendor comparison (NVIDIA vs AMD vs Intel)
 *   - Cross-driver-version comparison
 *   - When fast-math or FMA contraction is enabled
 *   - When multithreaded rasterization doesn't guarantee triangle order
 *
 * WHAT GRAPHICS APIs GUARANTEE (and don't):
 *   - GL/Vulkan: the `invariant` qualifier guarantees that the SAME shader
 *     with the SAME inputs produces the SAME output across draw calls.
 *     But NOT across different shaders or different compilers.
 *   - D3D: has IEEE strictness modes (precise/[precise])
 *   - NONE of them guarantee cross-vendor bit-identical results
 *   - Verification teams maintain PER-VENDOR golden images with tolerances
 *
 * HOW A VERIFICATION TEAM HANDLES THIS:
 *   1. Software reference rasterizer (THIS PROJECT) produces THE golden image
 *   2. Hardware output is compared against it with per-test tolerances
 *   3. Known-different areas (e.g., edge rasterization rule differences
 *      between vendors) are masked out
 *   4. Statistical thresholds: max differing pixels, max channel diff,
 *      PSNR, SSIM, etc.
 *   5. Any new failure is investigated — it might be a real hardware bug
 */

#include "framebuffer.h"
#include <iostream>
#include <cstdlib>
#include <cmath>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * COMPARISON RESULT
 * ═══════════════════════════════════════════════════════════════════════ */
struct CompareResult {
    bool exact_match;          // CRC32 hashes are identical
    int total_pixels;
    int differing_pixels;      // number of pixels with any channel difference
    int max_channel_diff;      // maximum single-channel difference (0-255)
    int first_diff_x;          // coordinates of the first mismatching pixel
    int first_diff_y;
    double psnr;               // Peak Signal-to-Noise Ratio (higher = more similar)

    void print() const {
        if (exact_match) {
            std::cout << "EXACT MATCH — images are bit-identical.\n";
            return;
        }
        std::cout << "MISMATCH REPORT:\n"
                  << "  Total pixels:       " << total_pixels << "\n"
                  << "  Differing pixels:   " << differing_pixels
                  << " (" << (100.0 * differing_pixels / total_pixels) << "%)\n"
                  << "  Max channel diff:   " << max_channel_diff << " / 255\n"
                  << "  First mismatch at:  (" << first_diff_x << ", " << first_diff_y << ")\n"
                  << "  PSNR:               " << psnr << " dB\n";
        if (psnr > 60.0)
            std::cout << "  Assessment: visually identical (PSNR > 60dB)\n";
        else if (psnr > 40.0)
            std::cout << "  Assessment: very similar (PSNR > 40dB)\n";
        else if (psnr > 30.0)
            std::cout << "  Assessment: noticeable differences\n";
        else
            std::cout << "  Assessment: significant differences — likely a bug\n";
    }
};

/* ═══════════════════════════════════════════════════════════════════════
 * COMPARISON FUNCTION
 * ═══════════════════════════════════════════════════════════════════════ */
inline CompareResult compareFramebuffers(const Framebuffer& a, const Framebuffer& b) {
    CompareResult r = {};
    r.total_pixels = a.width * a.height;
    r.first_diff_x = -1;
    r.first_diff_y = -1;

    if (a.width != b.width || a.height != b.height) {
        r.exact_match = false;
        r.differing_pixels = r.total_pixels;
        r.max_channel_diff = 255;
        r.psnr = 0.0;
        return r;
    }

    /* First: quick exact check via hash */
    if (a.colorHash() == b.colorHash()) {
        r.exact_match = true;
        r.psnr = 999.0; // infinite PSNR
        return r;
    }

    r.exact_match = false;
    double mse_sum = 0.0;

    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const Color& ca = a.colorAt(x, y);
            const Color& cb = b.colorAt(x, y);

            int dr = std::abs((int)ca.r - (int)cb.r);
            int dg = std::abs((int)ca.g - (int)cb.g);
            int db = std::abs((int)ca.b - (int)cb.b);
            int da = std::abs((int)ca.a - (int)cb.a);

            int maxd = std::max({dr, dg, db, da});
            if (maxd > 0) {
                r.differing_pixels++;
                r.max_channel_diff = std::max(r.max_channel_diff, maxd);
                if (r.first_diff_x < 0) {
                    r.first_diff_x = x;
                    r.first_diff_y = y;
                }
            }
            mse_sum += dr * dr + dg * dg + db * db;
        }
    }

    double mse = mse_sum / (a.width * a.height * 3.0);
    r.psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════
 * TOLERANCE CHECK — does the comparison pass a given tolerance?
 * ═══════════════════════════════════════════════════════════════════════ */
inline bool withinTolerance(const CompareResult& r,
                             int maxAllowedDiffPixels = 0,
                             int maxAllowedChannelDiff = 0) {
    if (r.exact_match) return true;
    return r.differing_pixels <= maxAllowedDiffPixels &&
           r.max_channel_diff <= maxAllowedChannelDiff;
}

} // namespace softras
