// Ft812Model: coprocessor execution into ram_dl, display-list decoding into Frame, CpuRaster.
// Words are encoded with the core's own macros (FT81x.h).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "Bitmaps.h"
#include "CompressionRLE.h"
#include "CpuRaster.h"
#include "FT81x.h"
#include "Ft812Model.h"
#include "LiberationSans-Regular_14_L4.h"
#include "test_images.h"

namespace
{
struct Chip
{
    Ft812Model m;

    void words(std::initializer_list<uint32_t> ws)
    {
        for (uint32_t w : ws)
            m.WriteData32(w);
    }
    // One coprocessor list: DLSTART, the words, SWAP.
    std::shared_ptr<const Frame> frame(std::initializer_list<uint32_t> ws)
    {
        m.beginCoProList();
        m.WriteData32(CMD_DLSTART);
        words(ws);
        m.WriteData32(CMD_SWAP);
        m.endCoProList();
        return m.latestFrame();
    }
};

uint32_t px(const std::vector<uint8_t>& rgb, int x, int y)
{
    const uint8_t* p = &rgb[(size_t(y) * ft812::kScreenWidth + x) * 3];
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}
} // namespace

TEST_CASE("test_points_and_lines")
{
    Chip c;
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 64), CLEAR(1, 1, 1), BEGIN(LINES),
                       COLOR_RGB(255, 0, 0), LINE_WIDTH(32), VERTEX2F(160, 160),
                       VERTEX2F(320, 160), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->clearColor == ft812::packRgba(0, 0, 64, 0));
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    const Prim& p = f->prims[0];
    REQUIRE(p.kind == Prim::Lines);
    REQUIRE(p.rgba == 0xFF0000FF);
    REQUIRE(p.lineRadiusPx == 2.0f);
    REQUIRE(p.verts.size() == 2);
    REQUIRE(p.verts[0] == Vec2{ 10, 10 });
    REQUIRE(p.verts[1] == Vec2{ 20, 10 });

    // points: radius = POINT_SIZE / 16
    f = c.frame({ POINT_SIZE(40), BEGIN(FTPOINTS), VERTEX2F(16, 32), VERTEX2F(48, 32), END(),
                  FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].kind == Prim::Points);
    REQUIRE(f->prims[0].pointRadiusPx == 2.5f);
    REQUIRE(f->prims[0].verts.size() == 2);
    REQUIRE(f->prims[0].verts[1] == Vec2{ 3, 2 });
    REQUIRE(f->prims[0].rgba == 0xFFFFFFFF); // context reset at the start of each list
    REQUIRE(f->serial > 0);
}

TEST_CASE("test_vertex_format_zero")
{
    Chip c;
    auto f = c.frame({ VERTEX_FORMAT(0), BEGIN(FTPOINTS), VERTEX2F(10, 10), VERTEX2F(-5, 479),
                       VERTEX_FORMAT(1), VERTEX2F(10, 10), VERTEX2II(300, 200, 3, 4), END(),
                       FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    const auto& v = f->prims[0].verts;
    REQUIRE(v.size() == 4);
    REQUIRE(v[0] == Vec2{ 10, 10 });
    REQUIRE(v[1] == Vec2{ -5, 479 }); // 15-bit signed field
    REQUIRE(v[2] == Vec2{ 5, 5 });
    REQUIRE(v[3] == Vec2{ 300, 200 }); // VERTEX2II is whole pixels whatever the format
}

TEST_CASE("test_save_restore_context")
{
    Chip c;
    auto f = c.frame({ COLOR_RGB(0, 255, 0), COLOR_A(128), LINE_WIDTH(48),
                       BLEND_FUNC(SRC_ALPHA, ONE), SAVE_CONTEXT(),
                       COLOR_RGB(255, 0, 0), COLOR_A(255), LINE_WIDTH(16), VERTEX_FORMAT(0),
                       BLEND_FUNC(SRC_ALPHA, ZERO), SCISSOR_XY(1, 2), SCISSOR_SIZE(3, 4),
                       BEGIN(LINES), VERTEX2F(1, 1), VERTEX2F(2, 2), END(), RESTORE_CONTEXT(),
                       BEGIN(LINES), VERTEX2F(16, 16), VERTEX2F(32, 32), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 2);
    const Prim& in = f->prims[0];
    REQUIRE(in.rgba == 0xFF0000FF);
    REQUIRE(in.lineRadiusPx == 1.0f);
    REQUIRE(in.blend == BlendMode{ SRC_ALPHA, ZERO });
    REQUIRE(in.scissor == Rect{ 1, 2, 3, 4 });
    REQUIRE(in.verts[1] == Vec2{ 2, 2 });

    const Prim& out = f->prims[1];
    REQUIRE(out.rgba == ft812::packRgba(0, 255, 0, 128));
    REQUIRE(out.lineRadiusPx == 3.0f);
    REQUIRE(out.blend == BlendMode{ SRC_ALPHA, ONE });
    REQUIRE(out.scissor == Rect{ 0, 0, 2048, 2048 });
    REQUIRE(out.verts[1] == Vec2{ 2, 2 }); // VERTEX_FORMAT restored to 4
}

TEST_CASE("test_scissor")
{
    Chip c;
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 255), CLEAR(1, 1, 1), VERTEX_FORMAT(0),
                       SCISSOR_XY(10, 20), SCISSOR_SIZE(30, 40), COLOR_RGB(255, 0, 0),
                       BEGIN(RECTS), VERTEX2F(0, 0), VERTEX2F(799, 479), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].kind == Prim::Rects);
    REQUIRE(f->prims[0].scissor == Rect{ 10, 20, 30, 40 });

    REQUIRE(f);
    auto rgb = CpuRaster::render(*f, c.m);
    REQUIRE(rgb.size() == size_t(ft812::kScreenWidth) * ft812::kScreenHeight * 3);
    REQUIRE(px(rgb, 5, 5) == 0x0000FF);
    REQUIRE(px(rgb, 10, 20) == 0xFF0000);
    REQUIRE(px(rgb, 39, 59) == 0xFF0000);
    REQUIRE(px(rgb, 40, 30) == 0x0000FF);
    REQUIRE(px(rgb, 20, 60) == 0x0000FF);
    REQUIRE(px(rgb, 9, 30) == 0x0000FF);
}

TEST_CASE("test_widget_cache_replay")
{
    Chip c;
    // widget update: expanded list lands in RAM_DL at dlOffset
    c.m.beginCoProListNoCheck();
    c.words({ CMD_DLSTART, COLOR_RGB(1, 2, 3), BEGIN(LINES), VERTEX2F(16, 16), VERTEX2F(160, 16),
              END() });
    c.m.endCoProList();
    const uint32_t off = c.m.getDisplayListOffset();
    REQUIRE(off == 5 * 4);

    // cache it like display.cpp step2
    c.m.beginCoProListNoCheck();
    c.words({ CMD_MEMCPY, 150000, RAM_DL, off });
    c.m.endCoProList();
    REQUIRE(std::memcmp(c.m.ramG() + 150000, c.m.ramDl(), off) == 0);

    // frame like display.cpp step3: append it twice
    c.m.beginCoProListNoCheck();
    c.words({ CMD_DLSTART, CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), CMD_APPEND, 150000, off,
              CMD_APPEND, 150000, off, FT_DISPLAY() });
    c.m.endCoProList();
    REQUIRE(c.m.getDisplayListOffset() == 2 * 4 + 2 * off + 4);
    c.m.beginCoProListNoCheck();
    c.words({ CMD_SWAP });
    c.m.endCoProList();

    auto f = c.m.latestFrame();
    REQUIRE(f);
    REQUIRE(f);
    REQUIRE(f->prims.size() == 2);
    for (const Prim& p : f->prims)
    {
        REQUIRE(p.kind == Prim::Lines);
        REQUIRE(p.rgba == ft812::packRgba(1, 2, 3, 255));
        REQUIRE(p.verts.size() == 2);
        REQUIRE(p.verts[1] == Vec2{ 10, 1 });
    }
}

TEST_CASE("frames are published per swap with increasing serials")
{
    Chip c;
    REQUIRE(c.m.latestFrame() == nullptr);
    auto a = c.frame({ FT_DISPLAY() });
    auto b = c.frame({ FT_DISPLAY() });
    REQUIRE(a != b);
    REQUIRE(b->serial == a->serial + 1);
    REQUIRE(a->prims.empty()); // a published frame is immutable
}

TEST_CASE("unknown coprocessor commands are skipped by their encoded length")
{
    Chip c;
    c.m.beginCoProList();
    c.words({ CMD_DLSTART, COLOR_RGB(9, 9, 9) });
    // Commands the core never sends, skipped by the length FT812.cpp encodes them with.
    // CMD_BUTTON: 16 bytes + NUL-terminated string padded to 4.
    c.words({ CMD_BUTTON, (10u << 16) | 20u, (5u << 16) | 50u, 1u });
    REQUIRE(c.m.sendString("hello", 0) == 8);
    c.words({ CMD_CLOCK, 0, 0, 0, 0, CMD_BGCOLOR, 0x00FF00, CMD_SPINNER, 0, 0 });
    // CMD_TEXT / CMD_NUMBER with a font no CMD_SETFONT(2) registered (a ROM font): nothing drawn
    c.words({ CMD_TEXT, (10u << 16) | 20u, (uint32_t(OPT_CENTERY) << 16) | 28u });
    REQUIRE(c.m.sendString("hello", 0) == 8);
    c.words({ CMD_NUMBER, 0, 28, 1234, CMD_LOADIDENTITY, CMD_ROTATE, 3 });
    c.words({ BEGIN(FTPOINTS), VERTEX2F(16, 16), END(), FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].rgba == ft812::packRgba(9, 9, 9, 255));
}

TEST_CASE("commands may straddle coprocessor lists")
{
    Chip c;
    c.m.beginCoProList();
    c.words({ CMD_DLSTART, CMD_MEMSET, 1000 });
    c.m.endCoProList();
    c.m.beginCoProList();
    c.words({ 0xAB, 6 });
    c.m.endCoProList();
    for (int i = 0; i < 6; i++)
        REQUIRE(c.m.ramG()[1000 + i] == 0xAB);
    REQUIRE(c.m.ramG()[1006] == 0);
}

TEST_CASE("memory commands and ram_g generations")
{
    Chip c;
    const uint32_t g0 = c.m.ramGGeneration(200000, 16);
    const uint32_t other0 = c.m.ramGGeneration(500000, 16);

    // MEMWRITE: payload follows inline, padded to 4
    c.m.beginCoProList();
    c.words({ CMD_MEMWRITE, 200000, 5, 0x44332211, 0x00000055 });
    c.m.endCoProList();
    REQUIRE(c.m.ramG()[200000] == 0x11);
    REQUIRE(c.m.ramG()[200004] == 0x55);
    REQUIRE(c.m.ramG()[200005] == 0);
    const uint32_t g1 = c.m.ramGGeneration(200000, 16);
    REQUIRE(g1 != g0);
    REQUIRE(c.m.ramGGeneration(500000, 16) == other0);

    c.m.beginCoProList();
    c.words({ CMD_MEMZERO, 200000, 2, CMD_MEMCPY, 300000, 200000, 5 });
    c.m.endCoProList();
    REQUIRE(c.m.ramG()[200000] == 0);
    REQUIRE(c.m.ramG()[200002] == 0x33);
    REQUIRE(c.m.ramG()[300002] == 0x33);
    REQUIRE(c.m.ramGGeneration(200000, 16) != g1);

    // host-side writes and reads
    const uint8_t img[6] = { 1, 2, 3, 4, 5, 6 };
    c.m.writeDataRAMG(img, 6, 400000);
    uint8_t back[6] = {};
    c.m.readDataRAMG(back, 6, 400000);
    REQUIRE(std::memcmp(back, img, 6) == 0);
    REQUIRE(c.m.ramGGeneration(399999, 2) != c.m.ramGGeneration(500000, 16));

    // font upload: bytes 144..147 become the glyph address (API_LIB_WriteFontRAMG)
    std::vector<uint8_t> font(160, 0x77);
    c.m.writeFontRAMG(font.data(), uint32_t(font.size()), 4000);
    uint32_t ptr;
    std::memcpy(&ptr, c.m.ramG() + 4000 + 144, 4);
    REQUIRE(ptr == 4000 + 148);
    REQUIRE(c.m.ramG()[4000 + 148] == 0x77);
}

TEST_CASE("bitmap prims resolve handle registers that persist across lists")
{
    Chip c;
    c.frame({ BITMAP_HANDLE(3), BITMAP_SOURCE(4096), BITMAP_LAYOUT(L4, 9, 17),
              BITMAP_SIZE(NEAREST, BORDER, BORDER, 18, 17), FT_DISPLAY() });
    auto f = c.frame({ VERTEX_FORMAT(0), BEGIN(BITMAPS), BITMAP_HANDLE(3), CELL(2),
                       VERTEX2F(100, 50), VERTEX2II(10, 20, 3, 'A'), END(),
                       BITMAP_TRANSFORM_A(128), PALETTE_SOURCE(777), BEGIN(BITMAPS),
                       VERTEX2F(0, 0), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 3);
    const Prim& a = f->prims[0];
    REQUIRE(a.kind == Prim::Bitmap);
    REQUIRE(a.verts.size() == 1);
    REQUIRE(a.verts[0] == Vec2{ 100, 50 });
    REQUIRE(a.handle == 3);
    REQUIRE(a.srcAddr == 4096);
    REQUIRE(a.fmt == L4);
    REQUIRE(a.stride == 9);
    REQUIRE(a.layoutH == 17);
    REQUIRE(a.w == 18);
    REQUIRE(a.h == 17);
    REQUIRE(a.cell == 2);
    REQUIRE(f->prims[1].cell == 'A');
    REQUIRE(f->prims[1].verts[0] == Vec2{ 10, 20 });
    REQUIRE(f->prims[2].xform[0] == 0.5f);
    REQUIRE(f->prims[2].xform[4] == 1.0f);
    REQUIRE(f->prims[2].paletteAddr == 777);
}

TEST_CASE("a state change inside an open primitive splits it")
{
    Chip c;
    auto f = c.frame({ VERTEX_FORMAT(0), BEGIN(LINE_STRIP), VERTEX2F(0, 0), VERTEX2F(10, 0),
                       COLOR_RGB(255, 0, 0), VERTEX2F(10, 10), END(), BEGIN(LINES), VERTEX2F(0, 0),
                       COLOR_RGB(0, 255, 0), VERTEX2F(5, 5), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 3);
    REQUIRE(f->prims[0].kind == Prim::LineStrip);
    REQUIRE(f->prims[0].verts.size() == 2);
    REQUIRE(f->prims[1].kind == Prim::LineStrip);
    REQUIRE(f->prims[1].rgba == 0xFF0000FF);
    REQUIRE(f->prims[1].verts.size() == 2); // continues from (10,0)
    REQUIRE(f->prims[1].verts[0] == Vec2{ 10, 0 });
    REQUIRE(f->prims[2].kind == Prim::Lines);
    REQUIRE(f->prims[2].rgba == 0x00FF00FF);
    REQUIRE(f->prims[2].verts.size() == 2);
}

TEST_CASE("cpu raster: rects, lines, points, blending")
{
    Chip c;
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), VERTEX_FORMAT(0),
                       // rect grown by line width 1 px: covers [9,21] x [9,21]
                       COLOR_RGB(255, 255, 255), LINE_WIDTH(16), BEGIN(RECTS), VERTEX2F(10, 10),
                       VERTEX2F(20, 20), END(),
                       // line at y = 100, radius 1 (LINE_WIDTH 16): covers rows 99 and 100
                       COLOR_RGB(0, 255, 0), BEGIN(LINES), VERTEX2F(100, 100), VERTEX2F(200, 100),
                       END(),
                       // disc radius 5 at (300, 300)
                       COLOR_RGB(0, 0, 255), POINT_SIZE(80), BEGIN(FTPOINTS), VERTEX2F(300, 300),
                       END(),
                       // half-transparent red over the white rect
                       COLOR_RGB(255, 0, 0), COLOR_A(128), BEGIN(RECTS), VERTEX2F(14, 14),
                       VERTEX2F(16, 16), END(), FT_DISPLAY() });
    REQUIRE(f);
    auto rgb = CpuRaster::render(*f, c.m);
    REQUIRE(px(rgb, 9, 9) != 0); // corner pixel partly covered (rounded)
    REQUIRE(px(rgb, 10, 10) == 0xFFFFFF);
    REQUIRE(px(rgb, 20, 15) == 0xFFFFFF); // (20,20) is a rounded corner pixel
    REQUIRE(px(rgb, 22, 15) == 0x000000);
    REQUIRE(px(rgb, 8, 15) == 0x000000);
    REQUIRE(px(rgb, 150, 99) == 0x00FF00);
    REQUIRE(px(rgb, 150, 100) == 0x00FF00);
    REQUIRE(px(rgb, 150, 98) == 0x000000);
    REQUIRE(px(rgb, 150, 101) == 0x000000);
    REQUIRE(px(rgb, 300, 300) == 0x0000FF);
    REQUIRE(px(rgb, 296, 300) == 0x0000FF);
    REQUIRE(px(rgb, 306, 300) == 0x000000);
    const uint32_t blended = px(rgb, 15, 15);
    REQUIRE(blended >> 16 == 0xFF);
    REQUIRE(((blended >> 8) & 0xFF) > 0x70);
    REQUIRE(((blended >> 8) & 0xFF) < 0x90);
}

// ---------------------------------------------------------------------------------------------
// Task 5b: coprocessor text, fonts, bitmaps, gradients, matrices, images
// ---------------------------------------------------------------------------------------------
namespace
{
constexpr uint32_t kFontAddr = 1000; // 4-aligned, like display.cpp loadFont
constexpr uint32_t kFontHandle = 1;

// The decompressed LiberationSans-Regular_14_L4 font: 148-byte metric block + glyphs.
std::vector<uint8_t> font14()
{
    const int n = int(sizeof(LiberationSans_Regular_14_L4_compressed) /
                      sizeof(LiberationSans_Regular_14_L4_compressed[0]));
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += LiberationSans_Regular_14_L4_compressed[i].count;
    std::vector<uint8_t> out(total);
    decompress(LiberationSans_Regular_14_L4_compressed, n, out.data());
    return out;
}

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

// Uploads the font and registers it exactly as cDisplay::loadFont does.
std::vector<uint8_t> loadFont(Chip& c)
{
    std::vector<uint8_t> f = font14();
    REQUIRE(f.size() > 148);
    uint32_t firstChar = 0;
    while (!f[firstChar])
        firstChar++;
    REQUIRE(firstChar == 32);
    const uint32_t stride = le32(&f[132]), w = le32(&f[136]), h = le32(&f[140]);
    REQUIRE(le32(&f[128]) == L4);
    c.m.writeFontRAMG(f.data(), uint32_t(f.size()), kFontAddr);
    c.frame({ BITMAP_HANDLE(kFontHandle), BITMAP_SOURCE(kFontAddr + 148),
              uint32_t(BITMAP_LAYOUT(L4, stride, h)),
              uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, w, h)), CMD_SETFONT2, kFontHandle,
              kFontAddr, firstChar, FT_DISPLAY() });
    return f;
}

// CMD_TEXT as FT812.cpp API_CMD_TEXT encodes it.
void text(Chip& c, int16_t x, int16_t y, uint32_t font, uint16_t options, const char* s)
{
    c.words({ CMD_TEXT, (uint32_t(uint16_t(y)) << 16) | uint16_t(x),
              (uint32_t(options) << 16) | font });
    c.m.sendString(s, 0);
}

std::vector<const Prim*> bitmaps(const Frame& f)
{
    std::vector<const Prim*> out;
    for (const Prim& p : f.prims)
        if (p.kind == Prim::Bitmap)
            out.push_back(&p);
    return out;
}
} // namespace

TEST_CASE("test_text_expands_to_dl_words")
{
    Chip c;
    const std::vector<uint8_t> font = loadFont(c);

    c.m.beginCoProList();
    c.words({ CMD_DLSTART });
    c.m.endCoProList();
    const uint32_t before = c.m.getDisplayListOffset();
    REQUIRE(before == 0);
    c.m.beginCoProList();
    text(c, 0, 0, kFontHandle, 0, "Ab");
    c.m.endCoProList();
    REQUIRE(c.m.getDisplayListOffset() > before); // expanded into ram_dl words
    c.m.beginCoProList();
    c.words({ FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();

    auto f = c.m.latestFrame();
    REQUIRE(f);
    const auto bm = bitmaps(*f);
    REQUIRE(bm.size() == 2);
    REQUIRE(bm[0]->handle == kFontHandle);
    REQUIRE(bm[0]->cell == 'A');
    REQUIRE(bm[1]->cell == 'b');
    REQUIRE(bm[0]->verts[0] == Vec2{ 0, 0 });
    REQUIRE(bm[1]->verts[0].x - bm[0]->verts[0].x == float(font['A']));
    REQUIRE(bm[0]->fmt == L4);

    // The glyph's texels are found: cell 'A' through the SETFONT2 source (gptr - 32 cells).
    const uint32_t cellBytes = bm[0]->stride * bm[0]->layoutH;
    REQUIRE(((bm[0]->srcAddr + 'A' * cellBytes) & 0x3FFFFF) == kFontAddr + 148 + 33 * cellBytes);

    // and drawn: some pixel inside the 'A' box is lit, white text on black
    auto rgb = CpuRaster::render(*f, c.m);
    int lit = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < int(font['A']); x++)
            lit += px(rgb, x, y) != 0;
    REQUIRE(lit > 10);
}

TEST_CASE("text options position the string and far coordinates use VERTEX2F")
{
    Chip c;
    const std::vector<uint8_t> font = loadFont(c);
    const float wAb = float(font['A'] + font['b']);
    const float h = float(le32(&font[140]));

    c.m.beginCoProList();
    c.words({ CMD_DLSTART, COLOR_RGB(255, 0, 0) });
    text(c, 100, 50, kFontHandle, OPT_RIGHTX, "Ab");
    text(c, 100, 50, kFontHandle, OPT_CENTER, "Ab");
    text(c, 700, 300, kFontHandle, 0, "Ab"); // x > 511: CELL + VERTEX2F
    c.words({ BEGIN(FTPOINTS), VERTEX2F(16, 16), END(), FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    const auto bm = bitmaps(*f);
    REQUIRE(bm.size() == 6);
    REQUIRE(bm[0]->verts[0] == Vec2{ 100 - wAb, 50 });
    REQUIRE(bm[2]->verts[0] == Vec2{ float(100 - int(wAb) / 2), float(50 - int(h) / 2) });
    REQUIRE(bm[4]->verts[0] == Vec2{ 700, 300 });
    REQUIRE(bm[4]->cell == 'A');
    REQUIRE(bm[5]->verts[0] == Vec2{ 700 + float(font['A']), 300 });
    REQUIRE(bm[5]->cell == 'b');
    for (const Prim* p : bm)
        REQUIRE(p->rgba == 0xFF0000FF);
    // the text left the context alone: the point after it is still in 1/16 px (VERTEX_FORMAT 4)
    REQUIRE(f->prims.back().kind == Prim::Points);
    REQUIRE(f->prims.back().verts[0] == Vec2{ 1, 1 });
}

TEST_CASE("test_cached_widget_keeps_text")
{
    Chip c;
    loadFont(c);
    // widget update with a CMD_TEXT inside
    c.m.beginCoProListNoCheck();
    c.words({ CMD_DLSTART, COLOR_RGB(1, 2, 3) });
    text(c, 10, 20, kFontHandle, 0, "Hi");
    c.words({ BEGIN(LINES), VERTEX2F(16, 16), VERTEX2F(160, 16), END() });
    c.m.endCoProList();
    const uint32_t off = c.m.getDisplayListOffset();

    c.m.beginCoProListNoCheck();
    c.words({ CMD_MEMCPY, 150000, RAM_DL, off });
    c.m.endCoProList();

    // a later frame only appends the cached copy
    c.m.beginCoProListNoCheck();
    c.words({ CMD_DLSTART, CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), CMD_APPEND, 150000, off,
              FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    const auto bm = bitmaps(*f);
    REQUIRE(bm.size() == 2);
    REQUIRE(bm[0]->cell == 'H');
    REQUIRE(bm[1]->cell == 'i');
    REQUIRE(bm[0]->verts[0] == Vec2{ 10, 20 });
    REQUIRE(bm[0]->rgba == ft812::packRgba(1, 2, 3, 255));
}

TEST_CASE("test_number_setbase")
{
    Chip c;
    loadFont(c);
    c.m.beginCoProList();
    c.words({ CMD_DLSTART, CMD_SETBASE, 16, CMD_NUMBER, (5u << 16) | 7u, kFontHandle, 255,
              CMD_SETBASE, 10, CMD_NUMBER, (40u << 16) | 7u, (uint32_t(OPT_SIGNED) << 16) |
              kFontHandle, uint32_t(-42), CMD_NUMBER, (80u << 16) | 7u, (3u << 16) | kFontHandle,
              7, FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    std::string s;
    for (const Prim* p : bitmaps(*f))
        s += char(p->cell);
    REQUIRE(s == "FF-42007"); // "FF", "-42", then 7 zero-padded to 3 digits
}

TEST_CASE("test_l4_decode")
{
    const uint8_t src[1] = { 0xF0 };
    const auto t = Bitmaps::decode(L4, src, 2, 1, 1, nullptr);
    REQUIRE(t.size() == 2);
    REQUIRE((t[0] & 0xFF) == 255);
    REQUIRE((t[1] & 0xFF) == 0);
    REQUIRE((t[0] >> 8) == 0xFFFFFF); // luminance formats are white with alpha = L
}

TEST_CASE("test_l2_decode")
{
    const uint8_t src[2] = { 0xE4, 0x40 }; // 3 2 1 0 | 1 0 0 0
    const auto t = Bitmaps::decode(L2, src, 5, 1, 2, nullptr);
    REQUIRE(t.size() == 5);
    REQUIRE(t[0] == 0xFFFFFFFF);
    REQUIRE(t[1] == 0xFFFFFFAA);
    REQUIRE(t[2] == 0xFFFFFF55);
    REQUIRE(t[3] == 0xFFFFFF00);
    REQUIRE(t[4] == 0xFFFFFF55);
}

TEST_CASE("test_paletted4444_decode")
{
    // one index byte per pixel; 16-bit ARGB4 palette entries (little endian)
    const uint8_t src[4] = { 0, 1, 2, 1 };
    const uint8_t palette[6] = { 0x00, 0x00, 0x0F, 0xF0, 0x84, 0x7C };
    const auto t = Bitmaps::decode(PALETTED4444, src, 2, 2, 2, palette);
    REQUIRE(t.size() == 4);
    REQUIRE(t[0] == 0x00000000);
    REQUIRE(t[1] == 0x0000FFFF); // 0xF00F: A=F R=0 G=0 B=F
    REQUIRE(t[2] == 0xCC884477); // 0x7C84: A=7 R=C G=8 B=4
    REQUIRE(t[3] == 0x0000FFFF);
}

TEST_CASE("other bitmap formats decode")
{
    const uint8_t rgb565[4] = { 0x00, 0xF8, 0xE0, 0x07 }; // red, green
    auto t = Bitmaps::decode(RGB565, rgb565, 2, 1, 4, nullptr);
    REQUIRE(t[0] == 0xFF0000FF);
    REQUIRE(t[1] == 0x00FF00FF);
    const uint8_t l8[2] = { 0x80, 0xFF };
    t = Bitmaps::decode(L8, l8, 2, 1, 2, nullptr);
    REQUIRE(t[0] == 0xFFFFFF80);
    const uint8_t l1[1] = { 0xA0 };
    t = Bitmaps::decode(L1, l1, 3, 1, 1, nullptr);
    REQUIRE(t[0] == 0xFFFFFFFF);
    REQUIRE(t[1] == 0xFFFFFF00);
    REQUIRE(t[2] == 0xFFFFFFFF);
    const uint8_t argb1555[2] = { 0x1F, 0x80 }; // A=1, blue
    t = Bitmaps::decode(ARGB1555, argb1555, 1, 1, 2, nullptr);
    REQUIRE(t[0] == 0x0000FFFF);
    const uint8_t rgb332[1] = { 0xE0 };
    t = Bitmaps::decode(RGB332, rgb332, 1, 1, 1, nullptr);
    REQUIRE(t[0] == 0xFF0000FF);
    const uint8_t pal8src[1] = { 1 };
    const uint8_t pal8[8] = { 0, 0, 0, 0, 0x30, 0x20, 0x10, 0x80 }; // B G R A
    t = Bitmaps::decode(PALETTED8, pal8src, 1, 1, 1, pal8);
    REQUIRE(t[0] == 0x10203080);
}

TEST_CASE("test_rotated_bitmap_transform")
{
    Chip c;
    // 4x4 L8 bitmap, only the top row lit
    std::vector<uint8_t> img(16, 0);
    for (int x = 0; x < 4; x++)
        img[x] = 255;
    c.m.writeDataRAMG(img.data(), 16, 20000);

    // label.cpp vertical text: rotate 270 degrees about the centre (2, 2)
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), BITMAP_HANDLE(7),
                       BITMAP_SOURCE(20000), uint32_t(BITMAP_LAYOUT(L8, 4, 4)),
                       uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 4, 4)), BEGIN(BITMAPS),
                       CMD_LOADIDENTITY, CMD_TRANSLATE, 2 * 65536, 2 * 65536, CMD_ROTATE,
                       270 * 65536 / 360, CMD_TRANSLATE, uint32_t(-2 * 65536),
                       uint32_t(-2 * 65536), CMD_SETMATRIX, VERTEX2II(100, 100, 7, 0), END(),
                       FT_DISPLAY() });
    REQUIRE(f);
    const auto bm = bitmaps(*f);
    REQUIRE(bm.size() == 1);
    // screen -> texel: u = -y + 4, v = x  (A..F as the coprocessor writes them, 8.8 fixed)
    const float* m = bm[0]->xform;
    REQUIRE(m[0] == 0.0f);
    REQUIRE(m[1] == -1.0f);
    REQUIRE(m[2] == 4.0f);
    REQUIRE(m[3] == 1.0f);
    REQUIRE(m[4] == 0.0f);
    REQUIRE(m[5] == 0.0f);

    // the top row of the bitmap ends up as the left column on screen
    auto rgb = CpuRaster::render(*f, c.m);
    for (int y = 100; y < 104; y++)
    {
        REQUIRE(px(rgb, 100, y) == 0xFFFFFF);
        REQUIRE(px(rgb, 101, y) == 0x000000);
        REQUIRE(px(rgb, 103, y) == 0x000000);
    }

    // CMD_SCALE(2): the bitmap appears twice as large (BITMAP_TRANSFORM_A/E = 128)
    f = c.frame({ CMD_LOADIDENTITY, CMD_SCALE, 2 * 65536, 2 * 65536, CMD_SETMATRIX,
                  BEGIN(BITMAPS), VERTEX2II(0, 0, 7, 0), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(bitmaps(*f).size() == 1);
    REQUIRE(bitmaps(*f)[0]->xform[0] == 0.5f);
    REQUIRE(bitmaps(*f)[0]->xform[4] == 0.5f);
}

TEST_CASE("bitmap texels are drawn with the colour, nearest and clipped to the size")
{
    Chip c;
    const uint8_t img[4] = { 0xFF, 0x00, 0x80, 0xFF }; // 2x2 L8
    c.m.writeDataRAMG(img, 4, 30000);
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), COLOR_RGB(0, 255, 0),
                       BITMAP_HANDLE(2), BITMAP_SOURCE(30000), uint32_t(BITMAP_LAYOUT(L8, 2, 2)),
                       uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 3, 2)), BEGIN(BITMAPS),
                       VERTEX2II(10, 10, 2, 0), END(),
                       // REPEAT: a 4x2 box shows the 2x2 bitmap twice
                       uint32_t(BITMAP_SIZE(NEAREST, REPEAT, BORDER, 4, 2)), BEGIN(BITMAPS),
                       VERTEX2II(20, 10, 2, 0), END(), FT_DISPLAY() });
    REQUIRE(f);
    auto rgb = CpuRaster::render(*f, c.m);
    REQUIRE(px(rgb, 10, 10) == 0x00FF00);
    REQUIRE(px(rgb, 11, 10) == 0x000000);
    REQUIRE(px(rgb, 10, 11) == 0x008000);
    REQUIRE(px(rgb, 12, 10) == 0x000000); // BORDER: outside the layout is transparent
    REQUIRE(px(rgb, 10, 12) == 0x000000); // outside BITMAP_SIZE
    REQUIRE(px(rgb, 22, 10) == 0x00FF00);
    REQUIRE(px(rgb, 23, 11) == 0x00FF00);
}

TEST_CASE("gradient expands to words that decode into a gradient prim")
{
    Chip c;
    c.m.beginCoProList();
    c.words({ CMD_DLSTART, CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), SCISSOR_XY(10, 100),
              SCISSOR_SIZE(50, 10) });
    c.m.endCoProList();
    const uint32_t before = c.m.getDisplayListOffset();
    REQUIRE(before == 4 * 4);
    // tracker.cpp shadow: vertical gradient from colour to black
    c.m.beginCoProList();
    c.words({ CMD_GRADIENT, (100u << 16) | 0u, 0xFF8000, (110u << 16) | 0u, 0x000000 });
    c.m.endCoProList();
    REQUIRE(c.m.getDisplayListOffset() == before + 5 * 4);
    // cached and replayed like any widget
    const uint32_t off = c.m.getDisplayListOffset();
    c.m.beginCoProList();
    c.words({ CMD_MEMCPY, 150000, RAM_DL, off, CMD_DLSTART, CMD_APPEND, 150000, off, FT_DISPLAY(),
              CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    const Prim& g = f->prims[0];
    REQUIRE(g.kind == Prim::Gradient);
    REQUIRE(g.rgba == 0xFF8000FF);
    REQUIRE(g.rgba2 == 0x000000FF);
    REQUIRE(g.verts.size() == 2);
    REQUIRE(g.verts[0] == Vec2{ 0, 100 });
    REQUIRE(g.verts[1] == Vec2{ 0, 110 });
    REQUIRE(g.scissor == Rect{ 10, 100, 50, 10 });

    auto rgb = CpuRaster::render(*f, c.m);
    REQUIRE(px(rgb, 9, 100) == 0x000000);   // outside the scissor
    REQUIRE(px(rgb, 30, 99) == 0x000000);
    REQUIRE(px(rgb, 30, 110) == 0x000000);
    const uint32_t top = px(rgb, 30, 100), mid = px(rgb, 30, 105), bottom = px(rgb, 30, 109);
    REQUIRE(top >> 16 > 0xE0);
    REQUIRE(mid >> 16 > 0x60);
    REQUIRE(mid >> 16 < 0xA0);
    REQUIRE(bottom >> 16 < 0x30);
    REQUIRE((top & 0xFF) == 0);
}

TEST_CASE("setbitmap writes the handle registers")
{
    Chip c;
    auto f = c.frame({ BITMAP_HANDLE(9), CMD_SETBITMAP, 40000, (600u << 16) | RGB565, 300,
                       BEGIN(BITMAPS), VERTEX2II(0, 0, 9, 0), END(), BITMAP_HANDLE(10),
                       CMD_SETBITMAP, 50000, (20u << 16) | L2, 16, BEGIN(BITMAPS),
                       VERTEX2II(0, 0, 10, 0), END(), FT_DISPLAY() });
    REQUIRE(f);
    const auto bm = bitmaps(*f);
    REQUIRE(bm.size() == 2);
    REQUIRE(bm[0]->srcAddr == 40000);
    REQUIRE(bm[0]->fmt == RGB565);
    REQUIRE(bm[0]->stride == 1200);
    REQUIRE(bm[0]->w == 600);
    REQUIRE(bm[0]->h == 300);
    REQUIRE(bm[0]->layoutH == 300);
    REQUIRE(bm[1]->fmt == L2);
    REQUIRE(bm[1]->stride == 5); // 20 px of 2 bits
    REQUIRE(bm[1]->w == 20);
}

TEST_CASE("snapshot2 calls the hook, or fills black headless")
{
    Chip c;
    c.m.writeDataRAMG(std::vector<uint8_t>(64, 0x55).data(), 64, 60000);
    c.m.beginCoProList();
    c.words({ CMD_SNAPSHOT2, 0x20, 60000, (1u << 16) | 2u, (3u << 16) | 8u }); // ARGB8, w 8 = 4 px
    c.m.endCoProList();
    for (int i = 0; i < 4 * 3 * 4; i += 4)
    {
        REQUIRE(c.m.ramG()[60000 + i] == 0);
        REQUIRE(c.m.ramG()[60000 + i + 3] == 0xFF);
    }
    REQUIRE(c.m.ramG()[60000 + 48] == 0x55);

    Rect seen;
    uint32_t seenAddr = 0, seenFmt = 0;
    c.m.snapshotHook = [&](Rect r, uint32_t addr, uint32_t fmt) {
        seen = r;
        seenAddr = addr;
        seenFmt = fmt;
        return true;
    };
    c.m.beginCoProList();
    c.words({ CMD_SNAPSHOT2, 0x20, 70000, (240u << 16) | 0u, (240u << 16) | 1600u });
    c.m.endCoProList();
    REQUIRE(seen == Rect{ 0, 240, 800, 240 });
    REQUIRE(seenAddr == 70000);
    REQUIRE(seenFmt == 0x20);
}

namespace
{
// The image viewer's sequence (display.cpp update): CMD_LOADIMAGE, then the file in 4-byte
// aligned chunks through writeDataToCMD (the last one padded), then CMD_GETPROPS.
void loadImage(Chip& c, const uint8_t* data, size_t size, uint32_t addr, uint32_t options)
{
    c.m.beginCoProListNoCheck();
    c.words({ CMD_LOADIMAGE, addr, options });
    c.m.endCoProList();
    const size_t chunk = 64;
    for (size_t at = 0; at < size; at += chunk)
        c.m.writeDataToCMD(data + at, uint32_t(std::min(chunk, size - at)));
    c.m.beginCoProList();
    c.words({ CMD_GETPROPS, 0, 0, 0 });
    c.m.endCoProList();
}
} // namespace

TEST_CASE("loadimage decodes a PNG into RGB565 and reports its properties")
{
    Chip c;
    loadImage(c, fixtures::kPng2x2, sizeof(fixtures::kPng2x2), 350000, OPT_NODL);
    uint32_t end = 0, w = 0, h = 0;
    c.m.getImgProps(end, w, h);
    REQUIRE(w == 2);
    REQUIRE(h == 2);
    REQUIRE(end == 350000 + 2 * 2 * 2);
    auto at = [&](int i) { return uint16_t(c.m.ramG()[350000 + 2 * i] |
                                           (c.m.ramG()[350000 + 2 * i + 1] << 8)); };
    REQUIRE(at(0) == 0xF800);
    REQUIRE(at(1) == 0x07E0);
    REQUIRE(at(2) == 0x001F);
    REQUIRE(at(3) == 0xFFFF);

    // the stream is still in sync: a list after the image draws normally
    auto f = c.frame({ BEGIN(FTPOINTS), VERTEX2F(32, 32), END(), FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].verts[0] == Vec2{ 2, 2 });
}

TEST_CASE("loadimage parses JPEG segments (FF D9 inside a segment does not end it)")
{
    Chip c;
    // without OPT_NODL the coprocessor also sets up the current bitmap handle
    c.m.beginCoProList();
    c.words({ CMD_DLSTART, BITMAP_HANDLE(4) });
    c.m.endCoProList();
    loadImage(c, fixtures::kJpeg16x8, sizeof(fixtures::kJpeg16x8), 360000, 0);
    uint32_t end = 0, w = 0, h = 0;
    c.m.getImgProps(end, w, h);
    REQUIRE(w == 16);
    REQUIRE(h == 8);
    REQUIRE(end == 360000 + 16 * 8 * 2);
    const uint16_t p = uint16_t(c.m.ramG()[360000] | (c.m.ramG()[360001] << 8));
    const int r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
    REQUIRE(r > 100);
    REQUIRE(r < 160);
    REQUIRE(g > 100);
    REQUIRE(b > 100);

    c.m.beginCoProList();
    c.words({ BEGIN(BITMAPS), VERTEX2II(0, 0, 4, 0), END(), FT_DISPLAY(), CMD_SWAP });
    c.m.endCoProList();
    auto f = c.m.latestFrame();
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].srcAddr == 360000);
    REQUIRE(f->prims[0].fmt == RGB565);
    REQUIRE(f->prims[0].stride == 32);
    REQUIRE(f->prims[0].w == 16);
    REQUIRE(f->prims[0].h == 8);
}

TEST_CASE("a scissored clear overwrites drawn pixels even with clear alpha 0")
{
    Chip c;
    auto f = c.frame({ VERTEX_FORMAT(0), BEGIN(RECTS), VERTEX2F(0, 0), VERTEX2F(100, 100), END(),
                       SCISSOR_XY(10, 10), SCISSOR_SIZE(20, 20), CLEAR_COLOR_RGB(0, 0, 255),
                       CLEAR(1, 1, 1), FT_DISPLAY() });
    REQUIRE(f);
    auto rgb = CpuRaster::render(*f, c.m);
    REQUIRE(px(rgb, 50, 50) == 0xFFFFFF);
    REQUIRE(px(rgb, 15, 15) == 0x0000FF); // (ONE, ZERO) writes the clear colour, alpha 0 included
}
