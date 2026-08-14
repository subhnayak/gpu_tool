/*
 * framebuffer.h — Color + Depth Framebuffer with Deterministic Output
 * =====================================================================
 *
 * LEARNING OBJECTIVES:
 *   - Understand what a framebuffer is and how it maps to GPU memory
 *   - Implement deterministic checksumming for golden-image verification
 *   - Output images without any external library dependency
 *
 * INTERVIEW: "How is a framebuffer organized in GPU memory?"
 * - Typically tiled (not linear) for cache locality during rasterization
 * - Color and depth are separate surfaces (MRT = Multiple Render Targets)
 * - Each pixel may have multiple samples (MSAA)
 * - GPU memory is banked across memory channels for parallel access
 *
 * For our software rasterizer, we use simple linear buffers — the focus
 * is on correctness, not memory layout optimization.
 */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <vector>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace softras {

/* ═══════════════════════════════════════════════════════════════════════
 * COLOR — 8-bit RGBA
 * ═══════════════════════════════════════════════════════════════════════
 * Real GPUs support many formats: R8G8B8A8_UNORM, R16G16B16A16_FLOAT,
 * R32G32B32A32_FLOAT, etc. We use 8-bit for simplicity and because
 * golden-image comparison at 8-bit is the common verification case.
 */
struct Color {
    uint8_t r, g, b, a;

    Color() : r(0), g(0), b(0), a(255) {}
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}

    /* Convert from float [0,1] to uint8. Clamp to avoid overflow.
     * Real GPUs do this in fixed-function "output merger" hardware. */
    static Color fromFloat(float fr, float fg, float fb, float fa = 1.0f) {
        auto clamp = [](float v) -> uint8_t {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<uint8_t>(v * 255.0f + 0.5f);
        };
        return {clamp(fr), clamp(fg), clamp(fb), clamp(fa)};
    }

    bool operator==(const Color& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

/* ═══════════════════════════════════════════════════════════════════════
 * CRC32 — Deterministic checksum for golden-image comparison
 * ═══════════════════════════════════════════════════════════════════════
 * We implement CRC32 here (no zlib dependency). This provides a
 * deterministic hash of the framebuffer contents for verification.
 *
 * WHY CRC32? It's fast, deterministic, and widely understood. For
 * verification, we don't need cryptographic strength — we need
 * bit-identical detection across runs and across optimization levels.
 */
inline uint32_t crc32_compute(const uint8_t* data, size_t length) {
    /* CRC32 lookup table — precomputed polynomial 0xEDB88320 (reversed) */
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j)
                crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
            table[i] = crc;
        }
        initialized = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ═══════════════════════════════════════════════════════════════════════
 * FRAMEBUFFER
 * ═══════════════════════════════════════════════════════════════════════
 */
class Framebuffer {
public:
    int width, height;
    std::vector<Color> color;
    std::vector<float> depth;

    Framebuffer() : width(0), height(0) {}
    Framebuffer(int w, int h) : width(w), height(h), color(w * h), depth(w * h, 1.0f) {}

    /* Clear to a solid color and reset depth.
     * INTERVIEW: "What does glClear actually do on the hardware?"
     * Modern GPUs can do a "fast clear" — they don't actually write every
     * pixel. Instead, they set a metadata flag per tile saying "this tile
     * is cleared to color X." The actual memory is only written when
     * something reads it. This saves massive bandwidth. */
    void clear(const Color& c = Color(0, 0, 0, 255), float d = 1.0f) {
        std::fill(color.begin(), color.end(), c);
        std::fill(depth.begin(), depth.end(), d);
    }

    /* Pixel access with bounds checking. */
    Color& colorAt(int x, int y) {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return color[y * width + x];
    }
    const Color& colorAt(int x, int y) const {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return color[y * width + x];
    }
    float& depthAt(int x, int y) {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return depth[y * width + x];
    }
    const float& depthAt(int x, int y) const {
        assert(x >= 0 && x < width && y >= 0 && y < height);
        return depth[y * width + x];
    }

    /* ═══════════════════════════════════════════════════════════════════
     * DETERMINISTIC HASH
     * ═══════════════════════════════════════════════════════════════════
     * CRC32 of the raw color buffer bytes. Used for golden-image comparison.
     * This hash MUST be identical across:
     *   - Multiple runs of the same program
     *   - Different optimization levels (-O0, -O2) IF we avoid fast-math
     *   - Different compilers IF we avoid implementation-defined behavior
     *
     * See DETERMINISM.md for the full discussion of what can break this.
     */
    uint32_t colorHash() const {
        return crc32_compute(reinterpret_cast<const uint8_t*>(color.data()),
                             color.size() * sizeof(Color));
    }

    /* Depth buffer hash — useful for verifying depth-related behavior. */
    uint32_t depthHash() const {
        return crc32_compute(reinterpret_cast<const uint8_t*>(depth.data()),
                             depth.size() * sizeof(float));
    }

    /* Hash as a hex string for display. */
    std::string colorHashHex() const {
        uint32_t h = colorHash();
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(8) << h;
        return oss.str();
    }

    /* ═══════════════════════════════════════════════════════════════════
     * PPM OUTPUT — Portable PixMap
     * ═══════════════════════════════════════════════════════════════════
     * PPM is the simplest possible image format: a header followed by raw
     * RGB bytes. No compression, no library dependency. Perfect for
     * verification output. Can be viewed with most image viewers.
     *
     * Format: "P6\n<width> <height>\n255\n" followed by width*height*3 bytes.
     */
    bool writePPM(const std::string& filename) const {
        std::ofstream f(filename, std::ios::binary);
        if (!f) return false;
        f << "P6\n" << width << " " << height << "\n255\n";
        for (int i = 0; i < width * height; ++i) {
            f.put(static_cast<char>(color[i].r));
            f.put(static_cast<char>(color[i].g));
            f.put(static_cast<char>(color[i].b));
        }
        return f.good();
    }

    /* ═══════════════════════════════════════════════════════════════════
     * BMP OUTPUT — Windows Bitmap
     * ═══════════════════════════════════════════════════════════════════
     * BMP with a 54-byte header, 24-bit BGR pixel data, 4-byte row padding.
     * More widely supported on Windows than PPM.
     */
    bool writeBMP(const std::string& filename) const {
        std::ofstream f(filename, std::ios::binary);
        if (!f) return false;

        int row_stride = ((width * 3 + 3) / 4) * 4; // pad rows to 4-byte boundary
        int data_size = row_stride * height;
        int file_size = 54 + data_size;

        /* BMP header */
        auto write16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
        auto write32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };

        f.put('B'); f.put('M');         // magic
        write32(file_size);             // file size
        write32(0);                     // reserved
        write32(54);                    // pixel data offset
        write32(40);                    // DIB header size
        write32(width);
        write32(height);
        write16(1);                     // planes
        write16(24);                    // bits per pixel
        write32(0);                     // compression (none)
        write32(data_size);
        write32(2835); write32(2835);   // pixels per meter (72 DPI)
        write32(0); write32(0);         // palette

        /* BMP stores rows bottom-to-top, BGR order */
        std::vector<uint8_t> row(row_stride, 0);
        for (int y = height - 1; y >= 0; --y) {
            for (int x = 0; x < width; ++x) {
                const Color& c = colorAt(x, y);
                row[x * 3 + 0] = c.b;
                row[x * 3 + 1] = c.g;
                row[x * 3 + 2] = c.r;
            }
            f.write(reinterpret_cast<char*>(row.data()), row_stride);
        }
        return f.good();
    }

    /* ═══════════════════════════════════════════════════════════════════
     * GOLDEN IMAGE COMPARISON
     * ═══════════════════════════════════════════════════════════════════
     * See golden_check.cpp for the full tolerance-based comparison.
     * This is a quick exact-match check via hash.
     */
    bool exactMatch(const Framebuffer& other) const {
        if (width != other.width || height != other.height) return false;
        return colorHash() == other.colorHash();
    }
};

} // namespace softras

#endif // FRAMEBUFFER_H
