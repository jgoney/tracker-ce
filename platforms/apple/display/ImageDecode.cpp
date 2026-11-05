#include "ImageDecode.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstring>

using ImageDecode::Scanner;

Scanner::Result Scanner::scan(const uint8_t* p, size_t n, size_t& length)
{
    if (kind_ == 0)
    {
        static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (n < 2)
            return NeedMore;
        if (p[0] == 0xFF && p[1] == 0xD8)
        {
            kind_ = 1;
            pos_ = 2;
        }
        else if (p[0] == 0x89)
        {
            if (n < 8)
                return NeedMore;
            if (std::memcmp(p, png, 8) != 0)
                return Invalid;
            kind_ = 2;
            pos_ = 8;
        }
        else
            return Invalid;
    }
    return kind_ == 1 ? scanJpeg(p, n, length) : scanPng(p, n, length);
}

Scanner::Result Scanner::scanJpeg(const uint8_t* p, size_t n, size_t& length)
{
    for (;;)
    {
        if (entropy_)
        {
            // Entropy-coded data: FF 00 is a stuffed byte, FF D0..D7 restart markers, FF FF fill.
            // Any other FF xx is the next marker.
            while (pos_ + 1 < n)
            {
                if (p[pos_] != 0xFF)
                {
                    pos_++;
                    continue;
                }
                const uint8_t m = p[pos_ + 1];
                if (m == 0x00 || (m >= 0xD0 && m <= 0xD7))
                    pos_ += 2;
                else if (m == 0xFF)
                    pos_++;
                else
                {
                    entropy_ = false;
                    break;
                }
            }
            if (entropy_)
                return NeedMore;
        }

        if (pos_ + 2 > n)
            return NeedMore;
        if (p[pos_] != 0xFF)
            return Invalid;
        const uint8_t m = p[pos_ + 1];
        if (m == 0xFF) // fill byte before a marker
        {
            pos_++;
            continue;
        }
        if (m == 0xD9) // EOI
        {
            length = pos_ + 2;
            return Done;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) // markers without a length
        {
            pos_ += 2;
            continue;
        }
        if (pos_ + 4 > n)
            return NeedMore;
        const size_t segment = (size_t(p[pos_ + 2]) << 8) | p[pos_ + 3];
        if (segment < 2)
            return Invalid;
        if (pos_ + 2 + segment > n)
            return NeedMore;
        pos_ += 2 + segment;
        if (m == 0xDA) // SOS: entropy-coded data follows the header
            entropy_ = true;
    }
}

Scanner::Result Scanner::scanPng(const uint8_t* p, size_t n, size_t& length)
{
    for (;;)
    {
        if (pos_ + 8 > n)
            return NeedMore;
        const size_t len = (size_t(p[pos_]) << 24) | (size_t(p[pos_ + 1]) << 16) |
                           (size_t(p[pos_ + 2]) << 8) | p[pos_ + 3];
        const bool iend = std::memcmp(p + pos_ + 4, "IEND", 4) == 0;
        const size_t next = pos_ + 12 + len; // length, type, data, CRC
        if (next > n)
            return NeedMore;
        pos_ = next;
        if (iend)
        {
            length = pos_;
            return Done;
        }
    }
}

bool ImageDecode::decode(const uint8_t* data,
                         size_t size,
                         bool mono,
                         std::vector<uint8_t>& out,
                         uint32_t& width,
                         uint32_t& height)
{
    width = height = 0;
    CFDataRef cfData = CFDataCreate(nullptr, data, CFIndex(size));
    if (!cfData)
        return false;
    CGImageSourceRef source = CGImageSourceCreateWithData(cfData, nullptr);
    CFRelease(cfData);
    if (!source)
        return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image)
        return false;

    const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    std::vector<uint8_t> rgba(w * h * 4, 0);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
      rgba.data(), w, h, 8, w * 4, rgb, uint32_t(kCGImageAlphaPremultipliedLast));
    CGColorSpaceRelease(rgb);
    if (!ctx)
    {
        CGImageRelease(image);
        return false;
    }
    // The context's origin is bottom-left but its memory is top row first, as EVE stores it.
    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), image);
    CGContextRelease(ctx);
    CGImageRelease(image);

    out.resize(w * h * (mono ? 1 : 2));
    for (size_t i = 0; i < w * h; i++)
    {
        const uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
        if (mono)
        {
            out[i] = uint8_t((r * 77 + g * 150 + b * 29 + 128) >> 8);
            continue;
        }
        const uint16_t v = uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        out[i * 2] = uint8_t(v);
        out[i * 2 + 1] = uint8_t(v >> 8);
    }
    width = uint32_t(w);
    height = uint32_t(h);
    return true;
}
