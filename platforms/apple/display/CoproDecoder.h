#pragma once
// Executes the FT812 coprocessor command stream against Ft812Model's memories, as EVE does:
// every command expands into display-list words at ram_dl[dlOffset]; CMD_SWAP publishes.
// Text, numbers, gradients, bitmap setup and matrices become display-list words (never prims),
// so widget lists cached with CMD_MEMCPY and replayed with CMD_APPEND keep them. Commands the
// core never sends are logged once and skipped by the length FT812.cpp encodes them with.
// See INVENTORY.md for every command and the semantics assumed.
#include "ImageDecode.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class Ft812Model;

class CoproDecoder
{
  public:
    explicit CoproDecoder(Ft812Model& model) : model_(model) {}

    // Appends bytes to the stream and executes every complete command. An incomplete trailing
    // command waits for more bytes (commands may straddle endCoProList / writeDataToCMD calls).
    void feed(const uint8_t* data, size_t len);

    // Bytes received but not yet executed (a partial command).
    size_t pendingBytes() const { return buf_.size() - pos_; }

  private:
    // Returns false if the command at pos_ is incomplete.
    bool step();
    uint32_t word(size_t at) const;
    bool execute(uint32_t w, size_t len);
    void skipUnknown(uint32_t opcode, size_t len);
    void badRange(uint32_t opcode);
    void writeDl(uint32_t w);
    // Returns the byte length of a NUL-terminated, 4-padded string at `at`, or 0 if incomplete.
    size_t paddedStringLen(size_t at) const;
    // Image data after CMD_LOADIMAGE: wait for the whole JPEG/PNG, decode it, skip its padding.
    bool loadImageData();

    // CMD_TEXT / CMD_NUMBER: glyphs as BEGIN(BITMAPS) + VERTEX2II (or CELL + VERTEX2F).
    void text(int32_t x, int32_t y, uint32_t font, uint32_t options, const char* s, size_t n);
    void number(int32_t x, int32_t y, uint32_t font, uint32_t options, int32_t n);
    // BITMAP_SOURCE / LAYOUT(_H) / SIZE(_H) for the current handle (CMD_SETBITMAP, LOADIMAGE).
    void setBitmap(uint32_t source, uint32_t fmt, uint32_t w, uint32_t h);
    void setFont2(uint32_t font, uint32_t ptr, uint32_t firstChar);
    void setMatrix();
    void snapshot(uint32_t fmt, uint32_t ptr, int32_t x, int32_t y, int32_t w, int32_t h);

    Ft812Model& model_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool loggedOnce_[256] = {};
    bool loggedRange_[256] = {};
    bool loggedOverflow_ = false;
    bool loggedFont_[32] = {};
    bool loggedChar_ = false;
    bool loggedMatrix_ = false;

    // CMD_LOADIMAGE in progress (its bytes stay in buf_ from pos_ until complete)
    bool inImage_ = false;
    uint32_t imagePtr_ = 0, imageOptions_ = 0;
    ImageDecode::Scanner scanner_;

    // Coprocessor state that persists across lists.
    struct Font
    {
        bool set = false;
        uint32_t metrics = 0;   // RAM_G address of the 148-byte metric block
        uint32_t firstChar = 0; // CMD_SETFONT2; 0 for CMD_SETFONT
    };
    Font fonts_[32];
    uint32_t base_ = 10; // CMD_SETBASE
    // Current matrix (CMD_LOADIDENTITY/TRANSLATE/SCALE/ROTATE), bitmap -> screen:
    // (x, y) -> (a x + b y + c, d x + e y + f).
    double m_[6] = { 1, 0, 0, 0, 1, 0 };
};
