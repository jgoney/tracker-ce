#pragma once
// FT81x bitmap formats: texel decoding to RGBA8 (0xRRGGBBAA, the Frame packing), and the layout
// arithmetic the coprocessor and the rasterisers share. See INVENTORY.md "Bitmap formats".
#include <cstdint>
#include <vector>

struct Prim;

namespace Bitmaps
{
// RAM_G range [addr, addr + bytes) holding the texels of a Bitmap prim's cell: cell n starts
// n * stride * height bytes after the source, 22-bit wrapped (CMD_SETFONT2 sources point before
// the first glyph). False if the layout is empty or the range leaves RAM_G (nothing is drawn).
bool cellRange(const Prim& p, uint32_t& addr, uint32_t& bytes);

// Decodes a w x h bitmap of format `fmt` (FT81x.h: ARGB1555, L1, L2, L4, L8, RGB332, ARGB2, ARGB4,
// RGB565, PALETTED565, PALETTED4444, PALETTED8) whose rows are `stride` bytes apart. `palette`
// is the PALETTE_SOURCE memory for the paletted formats (nullptr reads as zeros). Luminance
// formats decode to white with alpha = L, as EVE does. Unsupported formats decode transparent.
std::vector<uint32_t> decode(uint32_t fmt,
                             const uint8_t* src,
                             uint32_t w,
                             uint32_t h,
                             uint32_t stride,
                             const uint8_t* palette);

// Bits per pixel of `fmt`, or 0 if it is not a supported bitmap format.
uint32_t bitsPerPixel(uint32_t fmt);

// Width in pixels that a row of `stride` bytes holds.
uint32_t layoutWidth(uint32_t fmt, uint32_t stride);

// Bytes per row for `width` pixels (CMD_SETBITMAP, CMD_LOADIMAGE).
uint32_t strideFor(uint32_t fmt, uint32_t width);

// Bytes the palette of `fmt` occupies (256 entries), 0 for non-paletted formats.
uint32_t paletteBytes(uint32_t fmt);
} // namespace Bitmaps
