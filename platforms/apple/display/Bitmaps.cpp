#include "Bitmaps.h"

#include "FT81x.h"
#include "Frame.h"

namespace
{
// n-bit channel value to 8 bits, rounded (bit replication for 1, 2, 4 and 8 bits).
uint8_t expand(uint32_t v, uint32_t bits)
{
    const uint32_t max = (1u << bits) - 1;
    return uint8_t((v * 255 + max / 2) / max);
}

uint32_t rgb565(uint32_t v, uint8_t a = 255)
{
    return ft812::packRgba(expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5), a);
}

uint32_t argb4(uint32_t v)
{
    return ft812::packRgba(expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4),
                           expand(v >> 12, 4));
}

uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }

uint32_t luminance(uint8_t l) { return ft812::packRgba(255, 255, 255, l); }
} // namespace

uint32_t Bitmaps::bitsPerPixel(uint32_t fmt)
{
    switch (fmt)
    {
        case L1: return 1;
        case L2: return 2;
        case L4: return 4;
        case L8:
        case RGB332:
        case ARGB2:
        case PALETTED565:
        case PALETTED4444:
        case PALETTED8: return 8;
        case ARGB1555:
        case ARGB4:
        case RGB565: return 16;
        default: return 0;
    }
}

uint32_t Bitmaps::layoutWidth(uint32_t fmt, uint32_t stride)
{
    const uint32_t bpp = bitsPerPixel(fmt);
    return bpp ? stride * 8 / bpp : 0;
}

uint32_t Bitmaps::strideFor(uint32_t fmt, uint32_t width)
{
    return (width * bitsPerPixel(fmt) + 7) / 8;
}

uint32_t Bitmaps::paletteBytes(uint32_t fmt)
{
    switch (fmt)
    {
        case PALETTED565:
        case PALETTED4444: return 256 * 2;
        case PALETTED8: return 256 * 4;
        default: return 0;
    }
}

std::vector<uint32_t> Bitmaps::decode(uint32_t fmt,
                                      const uint8_t* src,
                                      uint32_t w,
                                      uint32_t h,
                                      uint32_t stride,
                                      const uint8_t* palette)
{
    std::vector<uint32_t> out(size_t(w) * h, 0);
    const uint32_t bpp = bitsPerPixel(fmt);
    if (!src || bpp == 0)
        return out;

    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* row = src + size_t(y) * stride;
        uint32_t* o = &out[size_t(y) * w];
        for (uint32_t x = 0; x < w; x++)
        {
            if (bpp < 8)
            {
                // sub-byte formats: leftmost pixel in the most significant bits
                const uint32_t bit = x * bpp;
                const uint32_t shift = 8 - bpp - (bit & 7);
                const uint32_t v = (row[bit >> 3] >> shift) & ((1u << bpp) - 1);
                o[x] = luminance(expand(v, bpp));
                continue;
            }
            const uint8_t* p = row + x * (bpp / 8);
            switch (fmt)
            {
                case L8: o[x] = luminance(p[0]); break;
                case RGB332:
                    o[x] = ft812::packRgba(
                      expand(p[0] >> 5, 3), expand((p[0] >> 2) & 7, 3), expand(p[0] & 3, 2), 255);
                    break;
                case ARGB2:
                    o[x] = ft812::packRgba(expand((p[0] >> 4) & 3, 2), expand((p[0] >> 2) & 3, 2),
                                           expand(p[0] & 3, 2), expand(p[0] >> 6, 2));
                    break;
                case ARGB1555:
                {
                    const uint32_t v = le16(p);
                    o[x] = ft812::packRgba(expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5),
                                           expand(v & 31, 5), (v & 0x8000) ? 255 : 0);
                    break;
                }
                case ARGB4: o[x] = argb4(le16(p)); break;
                case RGB565: o[x] = rgb565(le16(p)); break;
                case PALETTED565: o[x] = palette ? rgb565(le16(palette + 2 * p[0])) : 0; break;
                case PALETTED4444: o[x] = palette ? argb4(le16(palette + 2 * p[0])) : 0; break;
                case PALETTED8:
                {
                    // 32-bit ARGB8 entries, little endian: B, G, R, A
                    const uint8_t* e = palette ? palette + 4 * p[0] : nullptr;
                    o[x] = e ? ft812::packRgba(e[2], e[1], e[0], e[3]) : 0;
                    break;
                }
                default: break;
            }
        }
    }
    return out;
}

bool Bitmaps::cellRange(const Prim& p, uint32_t& addr, uint32_t& bytes)
{
    constexpr uint64_t kRamGSize = 1024 * 1024;
    if (layoutWidth(p.fmt, p.stride) == 0 || p.layoutH == 0)
        return false;
    bytes = p.stride * p.layoutH;
    addr = (p.srcAddr + p.cell * bytes) & 0x3FFFFF;
    return uint64_t(addr) + bytes <= kRamGSize;
}
