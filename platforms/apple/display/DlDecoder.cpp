#include "DlDecoder.h"

#include "FT81x.h"

#include <cstdio>

namespace
{
// Display-list opcodes (bits 31..24), FT81x.h macro encodings.
enum Op : uint32_t
{
    OpDisplay = 0,
    OpBitmapSource = 1,
    OpClearColorRgb = 2,
    OpTag = 3,
    OpColorRgb = 4,
    OpBitmapHandle = 5,
    OpCell = 6,
    OpBitmapLayout = 7,
    OpBitmapSize = 8,
    OpAlphaFunc = 9,
    OpStencilFunc = 10,
    OpBlendFunc = 11,
    OpStencilOp = 12,
    OpPointSize = 13,
    OpLineWidth = 14,
    OpClearColorA = 15,
    OpColorA = 16,
    OpClearStencil = 17,
    OpClearTag = 18,
    OpStencilMask = 19,
    OpTagMask = 20,
    OpTransformA = 21,
    OpTransformB = 22,
    OpTransformC = 23,
    OpTransformD = 24,
    OpTransformE = 25,
    OpTransformF = 26,
    OpScissorXY = 27,
    OpScissorSize = 28,
    OpCall = 29,
    OpJump = 30,
    OpBegin = 31,
    OpColorMask = 32,
    OpEnd = 33,
    OpSaveContext = 34,
    OpRestoreContext = 35,
    OpReturn = 36,
    OpMacro = 37,
    OpClear = 38,
    OpVertexFormat = 39,
    OpBitmapLayoutH = 40,
    OpBitmapSizeH = 41,
    OpPaletteSource = 42,
    OpVertexTranslateX = 43,
    OpVertexTranslateY = 44,
    OpNop = 45,
};

int32_t signExtend(uint32_t v, int bits)
{
    const uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return int32_t(v ^ m) - int32_t(m);
}

// Graphics context: everything SAVE_CONTEXT/RESTORE_CONTEXT preserves. Defaults are the state
// at the start of every display list.
struct Context
{
    uint8_t r = 255, g = 255, b = 255, a = 255;
    uint8_t clearR = 0, clearG = 0, clearB = 0, clearA = 0;
    uint32_t lineWidth = 16; // 1/16 px
    uint32_t pointSize = 16; // 1/16 px
    PixelRect scissor{ 0, 0, 2048, 2048 };
    BlendMode blend{ SRC_ALPHA, ONE_MINUS_SRC_ALPHA };
    uint32_t handle = 0, cell = 0;
    int32_t xf[6] = { 256, 0, 0, 0, 256, 0 }; // A,B,D,E: 8.8; C,F: 15.8
    uint32_t vertexFormat = 4;
    uint32_t palette = 0;
    int32_t translateX = 0, translateY = 0; // 1/16 px
    // Tracked but not represented in Frame (unused by the core).
    uint8_t colorMask = 0xF, alphaFunc = ALWAYS, alphaRef = 0;
    uint8_t stencilFunc = ALWAYS, stencilRef = 0, stencilFuncMask = 255;
    uint8_t stencilFail = KEEP, stencilPass = KEEP, stencilMask = 255;
    uint8_t clearStencil = 0, clearTag = 0, tag = 0, tagMask = 1;
};

class Decoder
{
  public:
    Decoder(DlDecoder::HandleRegs* handles, Frame& out) : handles_(handles), out_(out) {}

    void run(const uint8_t* dl, size_t bytes);

  private:
    uint32_t rgba() const { return ft812::packRgba(c_.r, c_.g, c_.b, c_.a); }
    Prim templ(Prim::Kind kind) const;
    bool sameState(const Prim& p) const;
    void vertex(Vec2 v, uint32_t handle, uint32_t cell);
    void clear(uint32_t w);
    void gradient(const uint32_t* args);

    DlDecoder::HandleRegs* handles_;
    Frame& out_;
    Context c_;
    Context stack_[4];
    int sp_ = 0;
    int begin_ = -1; // BEGIN primitive, -1 outside BEGIN/END
    bool open_ = false; // out_.prims.back() accepts more vertices
};

Prim Decoder::templ(Prim::Kind kind) const
{
    Prim p;
    p.kind = kind;
    p.rgba = rgba();
    p.lineRadiusPx = float(c_.lineWidth) / 16.0f;
    p.pointRadiusPx = float(c_.pointSize) / 16.0f;
    p.scissor = c_.scissor;
    p.blend = c_.blend;
    switch (begin_)
    {
        case EDGE_STRIP_L: p.edge = Prim::EdgeL; break;
        case EDGE_STRIP_A: p.edge = Prim::EdgeA; break;
        case EDGE_STRIP_B: p.edge = Prim::EdgeB; break;
        default: p.edge = Prim::EdgeR; break;
    }
    return p;
}

bool Decoder::sameState(const Prim& p) const
{
    return p.rgba == rgba() && p.lineRadiusPx == float(c_.lineWidth) / 16.0f &&
           p.pointRadiusPx == float(c_.pointSize) / 16.0f && p.scissor == c_.scissor &&
           p.blend == c_.blend;
}

void Decoder::vertex(Vec2 v, uint32_t handle, uint32_t cell)
{
    Prim::Kind kind;
    switch (begin_)
    {
        case BITMAPS: kind = Prim::Bitmap; break;
        case FTPOINTS: kind = Prim::Points; break;
        case LINES: kind = Prim::Lines; break;
        case LINE_STRIP: kind = Prim::LineStrip; break;
        case EDGE_STRIP_R:
        case EDGE_STRIP_L:
        case EDGE_STRIP_A:
        case EDGE_STRIP_B: kind = Prim::EdgeStrip; break;
        case RECTS: kind = Prim::Rects; break;
        default: return; // vertex outside BEGIN/END, or an unsupported primitive: draws nothing
    }

    if (kind == Prim::Bitmap)
    {
        // One prim per cell drawn; handle registers resolved now (they may change later).
        Prim p = templ(kind);
        const DlDecoder::HandleRegs& h = handles_[handle & 31];
        p.handle = handle & 31;
        p.srcAddr = h.source;
        p.fmt = h.format;
        p.stride = h.stride | (h.strideH << 10);
        p.layoutH = h.height | (h.heightH << 9);
        p.w = h.width | (h.widthH << 9);
        p.h = h.sizeHeight | (h.sizeHeightH << 9);
        if (p.w == 0)
            p.w = 2048;
        if (p.h == 0)
            p.h = 2048;
        p.filter = h.filter;
        p.wrapX = h.wrapX;
        p.wrapY = h.wrapY;
        p.cell = cell;
        p.paletteAddr = c_.palette;
        p.xform[0] = float(c_.xf[0]) / 256.0f;
        p.xform[1] = float(c_.xf[1]) / 256.0f;
        p.xform[2] = float(c_.xf[2]) / 256.0f;
        p.xform[3] = float(c_.xf[3]) / 256.0f;
        p.xform[4] = float(c_.xf[4]) / 256.0f;
        p.xform[5] = float(c_.xf[5]) / 256.0f;
        p.verts.push_back(v);
        out_.prims.push_back(std::move(p));
        open_ = false;
        return;
    }

    if (!open_ || !sameState(out_.prims.back()))
    {
        // New prim. A state change mid-primitive carries the pending vertex over: the dangling
        // first vertex of a LINES/RECTS pair, or the last strip vertex (the strip continues).
        Prim p = templ(kind);
        if (open_)
        {
            Prim& prev = out_.prims.back();
            const bool strip = kind == Prim::LineStrip || kind == Prim::EdgeStrip;
            const bool paired = kind == Prim::Lines || kind == Prim::Rects;
            if (strip && !prev.verts.empty())
                p.verts.push_back(prev.verts.back());
            else if (paired && (prev.verts.size() & 1))
            {
                p.verts.push_back(prev.verts.back());
                prev.verts.pop_back();
            }
            if (prev.verts.empty() || (strip && prev.verts.size() < 2))
                out_.prims.pop_back();
        }
        out_.prims.push_back(std::move(p));
        open_ = true;
    }
    out_.prims.back().verts.push_back(v);
}

void Decoder::clear(uint32_t w)
{
    if (!(w & 4)) // colour buffer not cleared (stencil/tag only)
        return;
    const uint32_t color = ft812::packRgba(c_.clearR, c_.clearG, c_.clearB, c_.clearA);
    const PixelRect& s = c_.scissor;
    const bool fullScreen = s.x <= 0 && s.y <= 0 && s.x + s.w >= ft812::kScreenWidth &&
                            s.y + s.h >= ft812::kScreenHeight;
    if (out_.prims.empty() && fullScreen)
    {
        out_.clearColor = color;
        return;
    }
    // A clear after drawing, or a scissored one: overwrite the scissor box.
    Prim p;
    p.kind = Prim::Rects;
    p.rgba = color;
    p.lineRadiusPx = 0;
    p.pointRadiusPx = 0;
    p.scissor = s;
    p.blend = BlendMode{ ONE, ZERO };
    p.verts = { Vec2{ float(s.x), float(s.y) }, Vec2{ float(s.x + s.w), float(s.y + s.h) } };
    out_.prims.push_back(std::move(p));
    open_ = false;
}

void Decoder::gradient(const uint32_t* args)
{
    auto point = [](uint32_t xy) {
        return Vec2{ float(int16_t(uint16_t(xy))), float(int16_t(uint16_t(xy >> 16))) };
    };
    // Gradient colours are opaque RGB (the 24-bit arguments); blend and scissor from the context.
    Prim p;
    p.kind = Prim::Gradient;
    p.rgba = (args[1] << 8) | 0xFF;
    p.rgba2 = (args[3] << 8) | 0xFF;
    p.lineRadiusPx = 0;
    p.pointRadiusPx = 0;
    p.scissor = c_.scissor;
    p.blend = c_.blend;
    p.verts = { point(args[0]), point(args[2]) };
    out_.prims.push_back(std::move(p));
    open_ = false;
}

void Decoder::run(const uint8_t* dl, size_t bytes)
{
    const size_t words = bytes / 4;
    size_t pc = 0;
    size_t callStack[4];
    int csp = 0;
    // A JUMP loop must not hang the host: no real list executes more than this.
    size_t budget = words * 8 + 64;

    while (pc < words && budget--)
    {
        const uint8_t* p = dl + pc * 4;
        const uint32_t w =
          uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        pc++;

        const uint32_t top = w >> 30;
        if (top == 1) // VERTEX2F
        {
            const float scale = 1.0f / float(1u << c_.vertexFormat);
            const float x = float(signExtend(w >> 15, 15)) * scale + float(c_.translateX) / 16.0f;
            const float y = float(signExtend(w, 15)) * scale + float(c_.translateY) / 16.0f;
            vertex(Vec2{ x, y }, c_.handle, c_.cell);
            continue;
        }
        if (top == 2) // VERTEX2II
        {
            const float x = float((w >> 21) & 511) + float(c_.translateX) / 16.0f;
            const float y = float((w >> 12) & 511) + float(c_.translateY) / 16.0f;
            vertex(Vec2{ x, y }, (w >> 7) & 31, w & 127);
            continue;
        }

        DlDecoder::HandleRegs& h = handles_[c_.handle & 31];
        switch (w >> 24)
        {
            case OpDisplay: return;
            case OpBitmapSource: h.source = w & 0x3FFFFF; break;
            case OpClearColorRgb:
                c_.clearR = uint8_t(w >> 16);
                c_.clearG = uint8_t(w >> 8);
                c_.clearB = uint8_t(w);
                break;
            case OpTag: c_.tag = uint8_t(w); break;
            case OpColorRgb:
                c_.r = uint8_t(w >> 16);
                c_.g = uint8_t(w >> 8);
                c_.b = uint8_t(w);
                break;
            case OpBitmapHandle: c_.handle = w & 31; break;
            case OpCell: c_.cell = w & 127; break;
            case OpBitmapLayout:
                h.format = (w >> 19) & 31;
                h.stride = (w >> 9) & 1023;
                h.height = w & 511;
                break;
            case OpBitmapSize:
                h.filter = (w >> 20) & 1;
                h.wrapX = (w >> 19) & 1;
                h.wrapY = (w >> 18) & 1;
                h.width = (w >> 9) & 511;
                h.sizeHeight = w & 511;
                break;
            case OpAlphaFunc:
                c_.alphaFunc = (w >> 8) & 7;
                c_.alphaRef = uint8_t(w);
                break;
            case OpStencilFunc:
                c_.stencilFunc = (w >> 16) & 7;
                c_.stencilRef = uint8_t(w >> 8);
                c_.stencilFuncMask = uint8_t(w);
                break;
            case OpBlendFunc: c_.blend = BlendMode{ uint8_t((w >> 3) & 7), uint8_t(w & 7) }; break;
            case OpStencilOp:
                c_.stencilFail = (w >> 3) & 7;
                c_.stencilPass = w & 7;
                break;
            case OpPointSize: c_.pointSize = w & 8191; break;
            case OpLineWidth: c_.lineWidth = w & 4095; break;
            case OpClearColorA: c_.clearA = uint8_t(w); break;
            case OpColorA: c_.a = uint8_t(w); break;
            case OpClearStencil: c_.clearStencil = uint8_t(w); break;
            case OpClearTag: c_.clearTag = uint8_t(w); break;
            case OpStencilMask: c_.stencilMask = uint8_t(w); break;
            case OpTagMask: c_.tagMask = w & 1; break;
            case OpTransformA: c_.xf[0] = signExtend(w, 17); break;
            case OpTransformB: c_.xf[1] = signExtend(w, 17); break;
            case OpTransformC: c_.xf[2] = signExtend(w, 24); break;
            case OpTransformD: c_.xf[3] = signExtend(w, 17); break;
            case OpTransformE: c_.xf[4] = signExtend(w, 17); break;
            case OpTransformF: c_.xf[5] = signExtend(w, 24); break;
            case OpScissorXY:
                c_.scissor.x = int((w >> 11) & 2047);
                c_.scissor.y = int(w & 2047);
                break;
            case OpScissorSize:
                c_.scissor.w = int((w >> 12) & 4095);
                c_.scissor.h = int(w & 4095);
                break;
            case OpCall:
                if (csp < 4)
                {
                    callStack[csp++] = pc;
                    pc = w & 0xFFFF;
                }
                break;
            case OpJump: pc = w & 0xFFFF; break;
            case OpReturn:
                if (csp == 0)
                    return;
                pc = callStack[--csp];
                break;
            case OpBegin:
                begin_ = int(w & 15);
                open_ = false;
                break;
            case OpColorMask: c_.colorMask = w & 15; break;
            case OpEnd:
                begin_ = -1;
                open_ = false;
                break;
            case OpSaveContext:
                if (sp_ < 4)
                    stack_[sp_++] = c_;
                break;
            case OpRestoreContext:
                if (sp_ > 0)
                    c_ = stack_[--sp_];
                break;
            case OpMacro:
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    std::fprintf(stderr, "Ft812Model: MACRO not modelled, ignored\n");
                }
                break;
            }
            case OpClear: clear(w); break;
            case OpVertexFormat: c_.vertexFormat = w & 7; break;
            case OpBitmapLayoutH:
                h.strideH = (w >> 2) & 3;
                h.heightH = w & 3;
                break;
            case OpBitmapSizeH:
                h.widthH = (w >> 2) & 3;
                h.sizeHeightH = w & 3;
                break;
            case OpPaletteSource: c_.palette = w & 0x3FFFFF; break;
            case OpVertexTranslateX: c_.translateX = signExtend(w, 17); break;
            case OpVertexTranslateY: c_.translateY = signExtend(w, 17); break;
            case DlDecoder::kGradientOp:
            {
                const size_t n = w & 0xFF;
                if (n == 4 && pc + 4 <= words)
                {
                    uint32_t args[4];
                    for (size_t i = 0; i < 4; i++)
                    {
                        const uint8_t* q = dl + (pc + i) * 4;
                        args[i] = uint32_t(q[0]) | (uint32_t(q[1]) << 8) |
                                  (uint32_t(q[2]) << 16) | (uint32_t(q[3]) << 24);
                    }
                    gradient(args);
                }
                pc += n;
                break;
            }
            case OpNop:
            default: break;
        }
    }
}
} // namespace

void DlDecoder::decode(const uint8_t* dl, size_t bytes, Frame& out)
{
    out.prims.clear();
    out.clearColor = ft812::packRgba(0, 0, 0, 255);
    Decoder d(handles_, out);
    d.run(dl, bytes);
    // Drop strips that never got a second vertex (draw nothing).
    std::vector<Prim> kept;
    kept.reserve(out.prims.size());
    for (Prim& p : out.prims)
    {
        const bool strip = p.kind == Prim::LineStrip || p.kind == Prim::EdgeStrip;
        const bool paired = p.kind == Prim::Lines || p.kind == Prim::Rects;
        if ((strip && p.verts.size() < 2) || (paired && p.verts.size() < 2) || p.verts.empty())
            continue;
        kept.push_back(std::move(p));
    }
    out.prims.swap(kept);
}
