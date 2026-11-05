// Ft812Model: coprocessor execution into ram_dl, display-list decoding into Frame, CpuRaster.
// Words are encoded with the core's own macros (FT81x.h).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "CpuRaster.h"
#include "FT81x.h"
#include "Ft812Model.h"

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
    REQUIRE(p.lineWidthPx == 2.0f);
    REQUIRE(p.verts.size() == 2);
    REQUIRE(p.verts[0] == Vec2{ 10, 10 });
    REQUIRE(p.verts[1] == Vec2{ 20, 10 });

    // points: radius = POINT_SIZE / 16
    f = c.frame({ POINT_SIZE(40), BEGIN(FTPOINTS), VERTEX2F(16, 32), VERTEX2F(48, 32), END(),
                  FT_DISPLAY() });
    REQUIRE(f);
    REQUIRE(f->prims.size() == 1);
    REQUIRE(f->prims[0].kind == Prim::Points);
    REQUIRE(f->prims[0].pointSizePx == 2.5f);
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
    REQUIRE(in.lineWidthPx == 1.0f);
    REQUIRE(in.blend == BlendMode{ SRC_ALPHA, ZERO });
    REQUIRE(in.scissor == Rect{ 1, 2, 3, 4 });
    REQUIRE(in.verts[1] == Vec2{ 2, 2 });

    const Prim& out = f->prims[1];
    REQUIRE(out.rgba == ft812::packRgba(0, 255, 0, 128));
    REQUIRE(out.lineWidthPx == 3.0f);
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
    // CMD_TEXT: 12 bytes + NUL-terminated string padded to 4 (FT812.cpp API_CMD_TEXT)
    c.words({ CMD_TEXT, (10u << 16) | 20u, (uint32_t(OPT_CENTERY) << 16) | 1u });
    REQUIRE(c.m.sendString("hello", 0) == 8);
    c.words({ CMD_GRADIENT, 0, 0, 0, 0x00FF00, CMD_NUMBER, 0, 0, 1234, CMD_SETBASE, 16,
              CMD_LOADIDENTITY, CMD_ROTATE, 3, CMD_SETMATRIX });
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
                       // 2 px wide horizontal line at y = 100 (radius 1): rows 99 and 100
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
