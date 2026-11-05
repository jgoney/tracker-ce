#include "CoproDecoder.h"

#include "FT81x.h"
#include "Ft812Model.h"

#include <cstdio>
#include <cstring>

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

bool CoproDecoder::swallowImage()
{
    // Consume LOADIMAGE payload until the end of the JPEG (FF D9) or PNG (IEND + CRC), then the
    // zero padding writeDataToCMD added to reach a word boundary. (A JPEG whose EXIF thumbnail
    // carries its own FF D9 would end early; Task 5b parses images properly.)
    while (pos_ < buf_.size())
    {
        const uint8_t b = buf_[pos_++];
        if (imageKind_ == 0)
            imageKind_ = (b == 0x89) ? 2 : 1;
        imageTail_ = (imageTail_ << 8) | b;
        bool done = false;
        if (imageKind_ == 1 && (imageTail_ & 0xFFFF) == 0xFFD9)
            done = true;
        if (imageKind_ == 2 && imageTail_ == 0x49454E44) // "IEND"
            imageKind_ = 3;                              // 4 CRC bytes follow
        else if (imageKind_ >= 3 && ++imageKind_ == 3 + 4)
            done = true;
        if (done)
        {
            inImage_ = false;
            imageKind_ = 0;
            imageTail_ = 0;
            pos_ = (pos_ + 3) & ~size_t(3);
            if (pos_ > buf_.size())
                pos_ = buf_.size();
            return true;
        }
    }
    return false;
}

bool CoproDecoder::step()
{
    if (inImage_)
        return swallowImage();

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

    const size_t a = pos_ + 4; // first argument
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
                writeDl(uint32_t(src[i]) | (uint32_t(src[i + 1]) << 8) |
                        (uint32_t(src[i + 2]) << 16) | (uint32_t(src[i + 3]) << 24));
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

        case CMD_LOADIMAGE:
            // Task 5b decodes the image; for now keep the stream in sync by eating its bytes.
            skipUnknown(w, len);
            inImage_ = true;
            return true;

        default: skipUnknown(w, len); return true;
    }
}
