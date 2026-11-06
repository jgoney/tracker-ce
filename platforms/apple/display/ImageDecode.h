#pragma once
// CMD_LOADIMAGE support: finding where a JPEG/PNG ends in the command stream, and decoding it
// (ImageIO/CoreGraphics, available on macOS and iOS) to the bitmap format EVE produces.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ImageDecode
{
// Walks a JPEG (marker segments, then entropy-coded data up to EOI) or a PNG (chunks up to
// IEND) as its bytes arrive, so a FF D9 inside an EXIF thumbnail or any other segment does not
// end the image early. Call scan() again with the same buffer after more bytes were appended.
class Scanner
{
  public:
    enum Result
    {
        NeedMore,
        Done,   // `length` is the image's byte length
        Invalid // neither JPEG nor PNG
    };

    Result scan(const uint8_t* p, size_t n, size_t& length);
    void reset() { *this = Scanner(); }

  private:
    Result scanJpeg(const uint8_t* p, size_t n, size_t& length);
    Result scanPng(const uint8_t* p, size_t n, size_t& length);

    int kind_ = 0;      // 0 unknown, 1 JPEG, 2 PNG
    size_t pos_ = 0;    // next byte to look at
    bool entropy_ = false; // JPEG: inside entropy-coded data after SOS
};

// Decodes a complete JPEG/PNG. `mono` (OPT_MONO): L8 luminance, otherwise RGB565 (little
// endian). Returns false if the data cannot be decoded.
bool decode(const uint8_t* data,
            size_t size,
            bool mono,
            std::vector<uint8_t>& out,
            uint32_t& width,
            uint32_t& height);
} // namespace ImageDecode
