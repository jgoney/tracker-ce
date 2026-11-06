#pragma once
// Decodes FT81x display-list words (FT81x.h encodings) into a Frame, tracking the graphics
// context (SAVE/RESTORE_CONTEXT) and the per-handle bitmap registers. See INVENTORY.md for the
// semantics assumed.
#include "Frame.h"

#include <cstddef>
#include <cstdint>

class DlDecoder
{
  public:
    // Bitmap handle registers: hardware state, not part of the context; they persist across
    // display lists (the core configures font handles once).
    struct HandleRegs
    {
        uint32_t source = 0;
        uint32_t format = 0, stride = 0, height = 0;    // BITMAP_LAYOUT (low bits)
        uint32_t strideH = 0, heightH = 0;              // BITMAP_LAYOUT_H
        uint8_t filter = 0, wrapX = 0, wrapY = 0;       // BITMAP_SIZE
        uint32_t width = 0, sizeHeight = 0;             // BITMAP_SIZE (low bits)
        uint32_t widthH = 0, sizeHeightH = 0;           // BITMAP_SIZE_H
    };
    static constexpr int kHandles = 32;

    // Model-private display-list instruction for CMD_GRADIENT (opcode 0x3E is unused by FT81x and
    // BT81x): the header word (kGradientOp << 24) | 4 is followed by the command's four argument
    // words verbatim: (y0 << 16 | x0), rgb0, (y1 << 16 | x1), rgb1. Decodes to a Gradient prim
    // over the current scissor.
    static constexpr uint32_t kGradientOp = 0x3E;
    static constexpr uint32_t kGradientHeader = (kGradientOp << 24) | 4;

    // Decodes words from `dl` (byte size `bytes`, little endian) until DISPLAY or the end.
    // `out.prims` and `out.clearColor` are overwritten; `out.serial` is left alone.
    void decode(const uint8_t* dl, size_t bytes, Frame& out);

    const HandleRegs& handle(int i) const { return handles_[i & (kHandles - 1)]; }

  private:
    HandleRegs handles_[kHandles];
};
