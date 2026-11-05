#include "CoproDecoder.h"

#include "Bitmaps.h"
#include "DlDecoder.h"
#include "FT81x.h"
#include "Ft812Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
// Byte length of each fixed-size coprocessor command as FT812.cpp encodes it (opcode included).
// 0 = not a command the core's encoder emits. String commands are handled separately.
size_t fixedLength(uint32_t op)
{
    switch (op)
    {
        case CMD_DLSTART:
        case CMD_SWAP:
        case CMD_LOADIDENTITY:
        case CMD_SETMATRIX:
        case CMD_STOP:
        case CMD_LOGO:
        case CMD_COLDSTART:
        case CMD_SCREENSAVER:
        case CMD_VIDEOSTART:
        case CMD_SYNC: return 4;
        case CMD_BGCOLOR:
        case CMD_FGCOLOR:
        case CMD_GRADCOLOR:
        case CMD_INFLATE:
        case CMD_ROTATE:
        case CMD_INTERRUPT:
        case CMD_CALIBRATE:
        case CMD_GETPTR:
        case CMD_SNAPSHOT:
        case CMD_SETROTATE:
        case CMD_PLAYVIDEO:
        case CMD_SETBASE:
        case CMD_SETSCRATCH: return 8;
        case CMD_APPEND:
        case CMD_MEMWRITE:
        case CMD_MEMZERO:
        case CMD_TRANSLATE:
        case CMD_SCALE:
        case CMD_SETFONT:
        case CMD_REGREAD:
        case CMD_LOADIMAGE:
        case CMD_SPINNER:
        case CMD_MEDIAFIFO:
        case CMD_INT_SWLOADIMAGE:
        case CMD_ROMFONT:
        case CMD_VIDEOFRAME: return 12;
        case CMD_MEMCPY:
        case CMD_MEMSET:
        case CMD_MEMCRC:
        case CMD_GETPROPS:
        case CMD_NUMBER:
        case CMD_DIAL:
        case CMD_TRACK:
        case CMD_SETFONT2:
        case CMD_SETBITMAP: return 16;
        case CMD_GRADIENT:
        case CMD_CLOCK:
        case CMD_GAUGE:
        case CMD_SLIDER:
        case CMD_SCROLLBAR:
        case CMD_PROGRESS:
        case CMD_SKETCH:
        case CMD_CSKETCH:
        case CMD_SNAPSHOT2: return 20;
        case CMD_GETMATRIX: return 28;
        case CMD_BITMAP_TRANSFORM: return 56;
        default: return 0;
    }
}

// Commands followed by a NUL-terminated string padded to 4: header length.
size_t stringHeaderLength(uint32_t op)
{
    switch (op)
    {
        case CMD_TEXT: return 12;
        case CMD_BUTTON:
        case CMD_KEYS:
        case CMD_TOGGLE: return 16;
        default: return 0;
    }
}

bool isCommand(uint32_t w) { return (w & 0xFFFFFF00u) == 0xFFFFFF00u; }

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

// Signed 16-bit halves of a packed (y << 16 | x) argument.
int32_t lo16(uint32_t w) { return int16_t(uint16_t(w)); }
int32_t hi16(uint32_t w) { return int16_t(uint16_t(w >> 16)); }

// Bytes per pixel of a CMD_SNAPSHOT2 format (0x20 = ARGB8 on FT81x).
uint32_t snapshotBpp(uint32_t fmt)
{
    if (fmt == 0x20)
        return 4;
    const uint32_t bpp = Bitmaps::bitsPerPixel(fmt);
    return bpp >= 8 ? bpp / 8 : 1;
}

constexpr uint32_t kSnapshotArgb8 = 0x20;
} // namespace

uint32_t CoproDecoder::word(size_t at) const
{
    return uint32_t(buf_[at]) | (uint32_t(buf_[at + 1]) << 8) | (uint32_t(buf_[at + 2]) << 16) |
           (uint32_t(buf_[at + 3]) << 24);
}

void CoproDecoder::feed(const uint8_t* data, size_t len)
{
    buf_.insert(buf_.end(), data, data + len);
    while (step())
    {
    }
    // keep only the unexecuted tail
    buf_.erase(buf_.begin(), buf_.begin() + long(pos_));
    pos_ = 0;
}

void CoproDecoder::writeDl(uint32_t w)
{
    uint32_t& off = model_.dlOffset();
    if (off + 4 > Ft812Model::kRamDlSize)
    {
        if (!loggedOverflow_)
        {
            loggedOverflow_ = true;
            std::fprintf(stderr, "Ft812Model: display list overflow (8 KB), words dropped\n");
        }
        return;
    }
    uint8_t* p = model_.ramDlMutable() + off;
    p[0] = uint8_t(w);
    p[1] = uint8_t(w >> 8);
    p[2] = uint8_t(w >> 16);
    p[3] = uint8_t(w >> 24);
    off += 4;
}

void CoproDecoder::skipUnknown(uint32_t opcode, size_t len)
{
    const uint8_t id = uint8_t(opcode);
    if (!loggedOnce_[id])
    {
        loggedOnce_[id] = true;
        std::fprintf(
            stderr,
            "Ft812Model: coprocessor command 0x%08X not modelled yet, skipped (%zu bytes)\n",
            opcode,
            len);
    }
    pos_ += len;
}

void CoproDecoder::badRange(uint32_t opcode)
{
    const uint8_t id = uint8_t(opcode);
    if (!loggedRange_[id])
    {
        loggedRange_[id] = true;
        std::fprintf(
            stderr,
            "Ft812Model: coprocessor command 0x%08X addresses memory outside RAM_G/RAM_DL, "
            "ignored\n",
            opcode);
    }
}

size_t CoproDecoder::paddedStringLen(size_t at) const
{
    for (size_t i = at; i < buf_.size(); i++)
    {
        if (buf_[i] == 0)
        {
            const size_t len = ((i - at + 1) + 3) & ~size_t(3);
            return at + len <= buf_.size() ? len : 0;
        }
    }
    return 0;
}

bool CoproDecoder::loadImageData()
{
    size_t length = 0;
    const size_t avail = buf_.size() - pos_;
    switch (scanner_.scan(&buf_[pos_], avail, length))
    {
        case ImageDecode::Scanner::NeedMore: return false;
        case ImageDecode::Scanner::Invalid:
            std::fprintf(stderr,
                         "Ft812Model: CMD_LOADIMAGE data is neither JPEG nor PNG, ignored\n");
            inImage_ = false;
            return true;
        case ImageDecode::Scanner::Done: break;
    }
    // The coprocessor resumes at the next word boundary (writeDataToCMD pads the last chunk).
    const size_t padded = (length + 3) & ~size_t(3);
    if (avail < padded)
        return false;

    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    const bool mono = imageOptions_ & OPT_MONO;
    if (!ImageDecode::decode(&buf_[pos_], length, mono, pixels, w, h))
    {
        std::fprintf(stderr, "Ft812Model: CMD_LOADIMAGE could not decode the image\n");
        model_.setImgProps(imagePtr_, 0, 0);
    }
    else if (uint8_t* d = model_.memAt(imagePtr_, uint32_t(pixels.size())))
    {
        std::memcpy(d, pixels.data(), pixels.size());
        model_.noteWrite(imagePtr_, uint32_t(pixels.size()));
        model_.setImgProps(imagePtr_ + uint32_t(pixels.size()), w, h);
        if (!(imageOptions_ & OPT_NODL))
            setBitmap(imagePtr_, mono ? L8 : RGB565, w, h);
    }
    else
        badRange(CMD_LOADIMAGE);
    pos_ += padded;
    inImage_ = false;
    return true;
}

void CoproDecoder::text(
  int32_t x, int32_t y, uint32_t font, uint32_t options, const char* s, size_t n)
{
    font &= 31;
    const Font& f = fonts_[font];
    const uint8_t* metrics = f.set ? model_.memAt(f.metrics, 148) : nullptr;
    if (!metrics)
    {
        // ROM fonts (16..31) are not modelled; the core only uses them in the factory test.
        if (!loggedFont_[font])
        {
            loggedFont_[font] = true;
            std::fprintf(stderr, "Ft812Model: text in unregistered font %u not drawn\n", font);
        }
        return;
    }

    int32_t width = 0;
    for (size_t i = 0; i < n; i++)
        if (uint8_t(s[i]) < 128)
            width += metrics[uint8_t(s[i])];
    const int32_t height = int32_t(le32(metrics + 140));
    if (options & OPT_CENTERX)
        x -= width / 2;
    else if (options & OPT_RIGHTX)
        x -= width;
    if (options & OPT_CENTERY)
        y -= height / 2;

    struct Glyph
    {
        int32_t x;
        uint32_t c;
    };
    std::vector<Glyph> glyphs;
    glyphs.reserve(n);
    bool far = y < 0 || y > 511; // VERTEX2II takes 0..511 only
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t c = uint8_t(s[i]);
        if (c >= 128)
        {
            // Cells are 7 bits; EVE's extended-font paging is not modelled.
            if (!loggedChar_)
            {
                loggedChar_ = true;
                std::fprintf(stderr, "Ft812Model: text characters >= 128 not drawn\n");
            }
            continue;
        }
        if (c >= f.firstChar && metrics[c] > 0)
        {
            glyphs.push_back({ x, c });
            far = far || x < 0 || x > 511;
        }
        x += metrics[c];
    }
    if (glyphs.empty())
        return;

    // Far glyphs need VERTEX2F, whose scale depends on VERTEX_FORMAT: pin it (and the handle
    // CELL goes with) inside SAVE/RESTORE_CONTEXT so the caller's context is untouched.
    if (far)
    {
        writeDl(SAVE_CONTEXT());
        writeDl(uint32_t(BITMAP_HANDLE(font)));
        writeDl(uint32_t(VERTEX_FORMAT(0)));
    }
    writeDl(uint32_t(BEGIN(BITMAPS)));
    for (const Glyph& g : glyphs)
    {
        if (g.x >= 0 && g.x <= 511 && y >= 0 && y <= 511)
            writeDl(uint32_t(VERTEX2II(uint32_t(g.x), uint32_t(y), font, g.c)));
        else
        {
            writeDl(uint32_t(CELL(g.c)));
            writeDl(uint32_t(VERTEX2F(uint32_t(g.x), uint32_t(y))));
        }
    }
    writeDl(uint32_t(END()));
    if (far)
        writeDl(RESTORE_CONTEXT());
}

void CoproDecoder::number(int32_t x, int32_t y, uint32_t font, uint32_t options, int32_t n)
{
    static const char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const uint32_t base = (base_ >= 2 && base_ <= 36) ? base_ : 10;
    const bool negative = (options & OPT_SIGNED) && n < 0;
    uint32_t v = negative ? uint32_t(0) - uint32_t(n) : uint32_t(n);
    std::string digits;
    do
    {
        digits.insert(digits.begin(), kDigits[v % base]);
        v /= base;
    } while (v);
    // FT81x: options bits 0..7 give a minimum digit count, padded with leading zeros.
    const size_t minDigits = std::min<size_t>(options & 0xFF, 32);
    while (digits.size() < minDigits)
        digits.insert(digits.begin(), '0');
    if (negative)
        digits.insert(digits.begin(), '-');
    text(x, y, font, options, digits.data(), digits.size());
}

void CoproDecoder::setBitmap(uint32_t source, uint32_t fmt, uint32_t w, uint32_t h)
{
    const uint32_t stride = Bitmaps::strideFor(fmt, w);
    writeDl(uint32_t(BITMAP_SOURCE(source)));
    writeDl(uint32_t(BITMAP_LAYOUT(fmt, stride, h)));
    writeDl(uint32_t(BITMAP_LAYOUT_H(stride >> 10, h >> 9)));
    writeDl(uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, w, h)));
    writeDl(uint32_t(BITMAP_SIZE_H(w >> 9, h >> 9)));
}

void CoproDecoder::setFont2(uint32_t font, uint32_t ptr, uint32_t firstChar)
{
    font &= 31;
    fonts_[font] = Font{ true, ptr, firstChar };
    const uint8_t* m = model_.memAt(ptr, 148);
    if (!m)
    {
        badRange(CMD_SETFONT2);
        return;
    }
    // The handle is set up so that the cell number is the character code: the source points
    // `firstChar` cells before the first glyph (22-bit address arithmetic wraps).
    const uint32_t fmt = le32(m + 128), stride = le32(m + 132), w = le32(m + 136),
                   h = le32(m + 140), glyphs = le32(m + 144);
    writeDl(uint32_t(BITMAP_HANDLE(font)));
    writeDl(uint32_t(BITMAP_SOURCE(glyphs - firstChar * stride * h)));
    writeDl(uint32_t(BITMAP_LAYOUT(fmt, stride, h)));
    writeDl(uint32_t(BITMAP_LAYOUT_H(stride >> 10, h >> 9)));
    writeDl(uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, w, h)));
    writeDl(uint32_t(BITMAP_SIZE_H(w >> 9, h >> 9)));
}

void CoproDecoder::setMatrix()
{
    // BITMAP_TRANSFORM maps screen to texel coordinates: the inverse of the current matrix.
    const double a = m_[0], b = m_[1], c = m_[2], d = m_[3], e = m_[4], f = m_[5];
    const double det = a * e - b * d;
    if (std::fabs(det) < 1e-12)
    {
        if (!loggedMatrix_)
        {
            loggedMatrix_ = true;
            std::fprintf(stderr, "Ft812Model: CMD_SETMATRIX with a singular matrix ignored\n");
        }
        return;
    }
    const double inv[6] = { e / det,  -b / det, (b * f - e * c) / det,
                            -d / det, a / det,  (d * c - a * f) / det };
    auto fx = [](double v) { return uint32_t(int32_t(std::lround(v * 256.0))); };
    writeDl(uint32_t(BITMAP_TRANSFORM_A(fx(inv[0]))));
    writeDl(uint32_t(BITMAP_TRANSFORM_B(fx(inv[1]))));
    writeDl(uint32_t(BITMAP_TRANSFORM_C(fx(inv[2]))));
    writeDl(uint32_t(BITMAP_TRANSFORM_D(fx(inv[3]))));
    writeDl(uint32_t(BITMAP_TRANSFORM_E(fx(inv[4]))));
    writeDl(uint32_t(BITMAP_TRANSFORM_F(fx(inv[5]))));
}

void CoproDecoder::snapshot(uint32_t fmt, uint32_t ptr, int32_t x, int32_t y, int32_t w, int32_t h)
{
    // ARGB8 snapshots take twice the pixel width (snapshots.cpp: 1600 for the 800 px screen).
    const Rect r{ x, y, fmt == kSnapshotArgb8 ? w / 2 : w, h };
    if (r.w <= 0 || r.h <= 0)
        return;
    const uint32_t bpp = snapshotBpp(fmt);
    const uint32_t bytes = uint32_t(r.w) * uint32_t(r.h) * bpp;
    uint8_t* d = model_.memAt(ptr, bytes);
    if (!d)
    {
        badRange(CMD_SNAPSHOT2);
        return;
    }
    if (!(model_.snapshotHook && model_.snapshotHook(r, ptr, fmt)))
    {
        // Headless: an opaque black screen in the requested format.
        uint32_t black = 0;
        if (fmt == kSnapshotArgb8)
            black = 0xFF000000u;
        else if (fmt == ARGB4)
            black = 0xF000;
        else if (fmt == ARGB1555)
            black = 0x8000;
        for (uint32_t i = 0; i < bytes; i += bpp)
            for (uint32_t k = 0; k < bpp; k++)
                d[i + k] = uint8_t(black >> (8 * k));
    }
    model_.noteWrite(ptr, bytes);
}

bool CoproDecoder::step()
{
    if (inImage_)
        return loadImageData();

    const size_t avail = buf_.size() - pos_;
    if (avail < 4)
        return false;
    const uint32_t w = word(pos_);

    if (!isCommand(w))
    {
        writeDl(w); // display-list word: copied to ram_dl as is
        pos_ += 4;
        return true;
    }

    if (size_t hdr = stringHeaderLength(w))
    {
        if (avail < hdr)
            return false;
        const size_t s = paddedStringLen(pos_ + hdr);
        if (s == 0)
            return false;
        if (w == CMD_TEXT)
        {
            const uint32_t xy = word(pos_ + 4), of = word(pos_ + 8);
            const char* str = reinterpret_cast<const char*>(&buf_[pos_ + hdr]);
            text(lo16(xy), hi16(xy), of & 0xFFFF, of >> 16, str, std::strlen(str));
            pos_ += hdr + s;
        }
        else
            skipUnknown(w, hdr + s);
        return true;
    }

    const size_t len = fixedLength(w);
    if (len == 0)
    {
        skipUnknown(w, 4); // not in the core's encoder: skip the opcode word only
        return true;
    }
    if (avail < len)
        return false;
    return execute(w, len);
}

bool CoproDecoder::execute(uint32_t w, size_t len)
{
    const size_t a = pos_ + 4; // first argument
    const size_t avail = buf_.size() - pos_;
    switch (w)
    {
        case CMD_DLSTART:
            model_.dlOffset() = 0;
            pos_ += len;
            return true;

        case CMD_SWAP:
            pos_ += len;
            model_.swap();
            return true;

        case CMD_APPEND:
        {
            const uint32_t ptr = word(a), num = word(a + 4) & ~3u;
            pos_ += len;
            const uint8_t* src = model_.memAt(ptr, num);
            if (!src)
            {
                badRange(w);
                return true;
            }
            for (uint32_t i = 0; i < num; i += 4)
                writeDl(le32(src + i));
            return true;
        }

        case CMD_MEMCPY:
        {
            const uint32_t dest = word(a), src = word(a + 4), num = word(a + 8);
            pos_ += len;
            uint8_t* d = model_.memAt(dest, num);
            const uint8_t* s = model_.memAt(src, num);
            if (d && s)
            {
                std::memmove(d, s, num);
                model_.noteWrite(dest, num);
            }
            else
                badRange(w);
            return true;
        }

        case CMD_MEMSET:
        case CMD_MEMZERO:
        {
            const uint32_t ptr = word(a);
            const uint8_t value = (w == CMD_MEMSET) ? uint8_t(word(a + 4)) : 0;
            const uint32_t num = (w == CMD_MEMSET) ? word(a + 8) : word(a + 4);
            pos_ += len;
            uint8_t* d = model_.memAt(ptr, num);
            if (d)
            {
                std::memset(d, value, num);
                model_.noteWrite(ptr, num);
            }
            else
                badRange(w);
            return true;
        }

        case CMD_MEMWRITE:
        {
            const uint32_t ptr = word(a), num = word(a + 4);
            const size_t payload = (size_t(num) + 3) & ~size_t(3);
            if (avail < len + payload)
                return false;
            uint8_t* d = model_.memAt(ptr, num);
            if (d)
            {
                std::memcpy(d, &buf_[pos_ + len], num);
                model_.noteWrite(ptr, num);
            }
            else
                badRange(w);
            pos_ += len + payload;
            return true;
        }

        case CMD_NUMBER:
        {
            const uint32_t xy = word(a), of = word(a + 4);
            const int32_t n = int32_t(word(a + 8));
            pos_ += len;
            number(lo16(xy), hi16(xy), of & 0xFFFF, of >> 16, n);
            return true;
        }

        case CMD_SETBASE:
            base_ = word(a);
            pos_ += len;
            return true;

        case CMD_SETFONT:
            fonts_[word(a) & 31] = Font{ true, word(a + 4), 0 };
            pos_ += len;
            return true;

        case CMD_SETFONT2:
        {
            const uint32_t font = word(a), ptr = word(a + 4), first = word(a + 8);
            pos_ += len;
            setFont2(font, ptr, first);
            return true;
        }

        case CMD_SETBITMAP:
        {
            const uint32_t source = word(a), wf = word(a + 4), h = word(a + 8);
            pos_ += len;
            setBitmap(source, wf & 0xFFFF, wf >> 16, h & 0xFFFF);
            return true;
        }

        case CMD_GRADIENT:
            // Model-private DL instruction carrying the arguments (DlDecoder::kGradientHeader).
            writeDl(DlDecoder::kGradientHeader);
            for (int i = 0; i < 4; i++)
                writeDl(word(a + 4 * size_t(i)));
            pos_ += len;
            return true;

        case CMD_LOADIDENTITY:
            m_[0] = 1, m_[1] = 0, m_[2] = 0, m_[3] = 0, m_[4] = 1, m_[5] = 0;
            pos_ += len;
            return true;

        case CMD_TRANSLATE:
        {
            const double tx = int32_t(word(a)) / 65536.0, ty = int32_t(word(a + 4)) / 65536.0;
            m_[2] += m_[0] * tx + m_[1] * ty;
            m_[5] += m_[3] * tx + m_[4] * ty;
            pos_ += len;
            return true;
        }

        case CMD_SCALE:
        {
            const double sx = int32_t(word(a)) / 65536.0, sy = int32_t(word(a + 4)) / 65536.0;
            m_[0] *= sx, m_[3] *= sx;
            m_[1] *= sy, m_[4] *= sy;
            pos_ += len;
            return true;
        }

        case CMD_ROTATE:
        {
            // Clockwise on screen (y down), in 1/65536 of a turn.
            const double t = int32_t(word(a)) * (2.0 * M_PI / 65536.0);
            const double cs = std::cos(t), sn = std::sin(t);
            const double a0 = m_[0], b0 = m_[1], d0 = m_[3], e0 = m_[4];
            m_[0] = a0 * cs + b0 * sn;
            m_[1] = b0 * cs - a0 * sn;
            m_[3] = d0 * cs + e0 * sn;
            m_[4] = e0 * cs - d0 * sn;
            pos_ += len;
            return true;
        }

        case CMD_SETMATRIX:
            pos_ += len;
            setMatrix();
            return true;

        case CMD_GETPROPS:
            // The results (end address, width, height) are what getImgProps() returns.
            pos_ += len;
            return true;

        case CMD_SNAPSHOT2:
        {
            const uint32_t fmt = word(a), ptr = word(a + 4), xy = word(a + 8), wh = word(a + 12);
            pos_ += len;
            snapshot(fmt, ptr, lo16(xy), hi16(xy), lo16(wh), hi16(wh));
            return true;
        }

        case CMD_LOADIMAGE:
            imagePtr_ = word(a);
            imageOptions_ = word(a + 4);
            pos_ += len;
            scanner_.reset();
            inImage_ = true;
            return true;

        default: skipUnknown(w, len); return true;
    }
}
