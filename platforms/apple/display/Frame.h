#pragma once
// One decoded FT812 display list: what the screen shows after a CMD_SWAP, as a list of
// primitives in pixel space. Produced by DlDecoder, consumed by CpuRaster (reference) and the
// Metal renderer. Bitmap texels stay in Ft812Model's RAM_G and are referenced by address.
#include <cstdint>
#include <vector>

namespace ft812
{
constexpr int kScreenWidth = 800;
constexpr int kScreenHeight = 480;

// rgba packing used throughout: 0xRRGGBBAA
constexpr uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
}
} // namespace ft812

struct Vec2
{
    float x = 0, y = 0;
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

// Pixel rectangle (EVE scissor: x, y, width, height in whole pixels).
struct PixelRect
{
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PixelRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// EVE BLEND_FUNC factors (FT81x.h: ZERO 0, ONE 1, SRC_ALPHA 2, DST_ALPHA 3,
// ONE_MINUS_SRC_ALPHA 4, ONE_MINUS_DST_ALPHA 5).
struct BlendMode
{
    uint8_t src = 2, dst = 4;
    bool operator==(const BlendMode& o) const { return src == o.src && dst == o.dst; }
};

struct Prim
{
    enum Kind
    {
        Points,
        Lines,     // vertex pairs
        LineStrip, // connected
        EdgeStrip, // edge: one of EdgeR/L/A/B
        Rects,     // vertex pairs (opposite corners)
        Bitmap,    // one vertex = top-left of one bitmap cell
        Gradient   // CMD_GRADIENT: verts[0] (rgba) to verts[1] (rgba2), fills the scissor
    } kind = Points;
    enum Edge
    {
        EdgeR,
        EdgeL,
        EdgeA,
        EdgeB
    } edge = EdgeR;

    uint32_t rgba = 0xFFFFFFFF; // 0xRRGGBBAA
    uint32_t rgba2 = 0;         // Gradient only: colour at verts[1]
    float lineRadiusPx = 1;     // LINE_WIDTH / 16: line radius, rect corner radius
    float pointRadiusPx = 1;    // POINT_SIZE / 16: point radius
    PixelRect scissor{ 0, 0, 2048, 2048 };
    BlendMode blend;
    std::vector<Vec2> verts; // px

    // Bitmap only (handle properties resolved at decode time).
    uint32_t handle = 0;
    uint32_t srcAddr = 0;    // BITMAP_SOURCE (cell 0)
    uint32_t fmt = 0;        // BITMAP_LAYOUT format
    uint32_t w = 0, h = 0;   // BITMAP_SIZE (drawn size, px)
    uint32_t stride = 0;     // BITMAP_LAYOUT line stride (bytes)
    uint32_t layoutH = 0;    // BITMAP_LAYOUT height (rows per cell)
    uint32_t cell = 0;
    uint32_t paletteAddr = 0;
    uint8_t filter = 0, wrapX = 0, wrapY = 0;
    // BITMAP_TRANSFORM A..F as reals: texel = (A x + B y + C, D x + E y + F) for screen offset
    // (x, y) from the vertex.
    float xform[6] = { 1, 0, 0, 0, 1, 0 };
};

struct Frame
{
    uint32_t clearColor = 0x000000FF; // 0xRRGGBBAA
    std::vector<Prim> prims;
    uint64_t serial = 0;
};
