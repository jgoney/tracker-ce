// MetalRenderer against CpuRaster: synthetic frames built through the real coprocessor model are
// rendered with Metal and compared with golden PNGs (CpuRaster output, reviewed and committed under
// tests/golden/). A missing golden is written from CpuRaster and the test fails, printing its path.
#include <catch2/catch_test_macros.hpp>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "CompressionRLE.h"
#include "CpuRaster.h"
#include "FT81x.h"
#include "Ft812Model.h"
#include "LiberationSans-Regular_14_L4.h"
#include "MetalRenderer.h"

namespace
{
constexpr int W = ft812::kScreenWidth, H = ft812::kScreenHeight;

struct Chip
{
    Ft812Model m;
    void words(std::initializer_list<uint32_t> ws)
    {
        for (uint32_t w : ws)
            m.WriteData32(w);
    }
    void begin()
    {
        m.beginCoProList();
        m.WriteData32(CMD_DLSTART);
    }
    std::shared_ptr<const Frame> end()
    {
        m.WriteData32(FT_DISPLAY());
        m.WriteData32(CMD_SWAP);
        m.endCoProList();
        return m.latestFrame();
    }
    std::shared_ptr<const Frame> frame(std::initializer_list<uint32_t> ws)
    {
        begin();
        words(ws);
        return end();
    }
    void text(int16_t x, int16_t y, uint32_t font, uint16_t options, const char* s)
    {
        words({ CMD_TEXT, (uint32_t(uint16_t(y)) << 16) | uint16_t(x),
                (uint32_t(options) << 16) | font });
        m.sendString(s, 0);
    }
};

uint32_t v16(float x, float y) { return VERTEX2F(int(std::lround(x * 16)), int(std::lround(y * 16))); }

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

constexpr uint32_t kFontAddr = 1000, kFontHandle = 1;

// LiberationSans-Regular_14_L4 uploaded and registered like cDisplay::loadFont. Returns the
// font data (metric widths at 0..127, height at 140).
std::vector<uint8_t> loadFont(Chip& c)
{
    const int n = int(sizeof(LiberationSans_Regular_14_L4_compressed) /
                      sizeof(LiberationSans_Regular_14_L4_compressed[0]));
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += LiberationSans_Regular_14_L4_compressed[i].count;
    std::vector<uint8_t> f(total);
    decompress(LiberationSans_Regular_14_L4_compressed, n, f.data());
    const uint32_t stride = le32(&f[132]), w = le32(&f[136]), h = le32(&f[140]);
    c.m.writeFontRAMG(f.data(), uint32_t(f.size()), kFontAddr);
    c.frame({ BITMAP_HANDLE(kFontHandle), BITMAP_SOURCE(kFontAddr + 148),
              uint32_t(BITMAP_LAYOUT(L4, stride, h)),
              uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, w, h)), CMD_SETFONT2, kFontHandle,
              kFontAddr, 32 });
    return f;
}

// label.cpp controlStyleVerticalText: glyph cells rotated 270 degrees about their centre, stacked
// upwards from (x, y).
void verticalText(Chip& c, const std::vector<uint8_t>& font, int16_t x, int16_t y, const char* s)
{
    const int32_t h = int32_t(le32(&font[140]));
    c.words({ SAVE_CONTEXT(), BITMAP_HANDLE(kFontHandle), BEGIN(BITMAPS),
              uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, h, h)), CMD_LOADIDENTITY,
              CMD_TRANSLATE, uint32_t(65536 * (h / 2)), uint32_t(65536 * (h / 2)), CMD_ROTATE,
              270 * 65536 / 360, CMD_TRANSLATE, uint32_t(65536 * (-h / 2)),
              uint32_t(65536 * (-h / 2)), CMD_SETMATRIX, BITMAP_HANDLE(kFontHandle),
              BEGIN(BITMAPS) });
    for (; *s; s++)
    {
        c.words({ uint32_t(VERTEX2II(uint32_t(x), uint32_t(y), kFontHandle, uint32_t(*s))) });
        y = int16_t(y - font[size_t(*s)] - 1);
    }
    c.words({ END(), RESTORE_CONTEXT(), CMD_LOADIDENTITY });
}

// ---- scenes ----

std::shared_ptr<const Frame> sceneLinesPoints(Chip& c)
{
    c.begin();
    c.words({ CLEAR_COLOR_RGB(16, 24, 48), CLEAR(1, 1, 1),
              // lines: radius 0.5, 1, 2, 5 px; horizontal, diagonal, sub-pixel ends
              BEGIN(LINES), COLOR_RGB(255, 255, 255), LINE_WIDTH(8), v16(20, 20.5f), v16(300, 20.5f),
              LINE_WIDTH(16), v16(20, 40), v16(300, 52), COLOR_RGB(255, 80, 40), LINE_WIDTH(32),
              v16(20.3f, 70.7f), v16(300.6f, 140.2f), COLOR_RGB(80, 200, 255), LINE_WIDTH(80),
              v16(40, 180), v16(120, 260), END(),
              // a translucent line strip over them
              COLOR_RGB(255, 220, 0), COLOR_A(160), LINE_WIDTH(24), BEGIN(LINE_STRIP),
              v16(10, 100), v16(80, 30), v16(160, 200), v16(260, 60), v16(330, 250), END(),
              COLOR_A(255),
              // points: radius 1, 2.5, 6.25, 20 px, overlapping translucent discs
              BEGIN(FTPOINTS), COLOR_RGB(255, 255, 255), POINT_SIZE(16), v16(400, 30),
              POINT_SIZE(40), v16(420.5f, 30.5f), POINT_SIZE(100), v16(450.25f, 30.75f),
              COLOR_RGB(255, 0, 0), COLOR_A(128), POINT_SIZE(320), v16(500, 120), v16(530, 130),
              COLOR_RGB(0, 255, 0), v16(515, 150), END(), COLOR_A(255),
              // additive blend (SRC_ALPHA, ONE)
              BLEND_FUNC(SRC_ALPHA, ONE), COLOR_RGB(0, 0, 200), BEGIN(FTPOINTS), POINT_SIZE(400),
              v16(620, 120), v16(660, 120), END(), BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA),
              // replace blend (ONE, ZERO): the translucent colour overwrites, alpha and all
              BLEND_FUNC(ONE, ZERO), COLOR_RGB(200, 100, 255), COLOR_A(100), LINE_WIDTH(48),
              BEGIN(LINES), v16(560, 200), v16(760, 220), END(), COLOR_A(255),
              // ... then DST_ALPHA reads that alpha back
              BLEND_FUNC(DST_ALPHA, ONE_MINUS_DST_ALPHA), COLOR_RGB(255, 255, 255), BEGIN(LINES),
              LINE_WIDTH(16), v16(580, 180), v16(740, 240), END(),
              BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA),
              // edge strips: right of a slanted polyline, below a zig-zag
              COLOR_RGB(60, 160, 60), BEGIN(EDGE_STRIP_R), v16(700, 260), v16(640.5f, 330),
              v16(720, 400), END(), COLOR_RGB(160, 60, 160), COLOR_A(200), BEGIN(EDGE_STRIP_B),
              v16(20, 400), v16(120, 360.5f), v16(220, 420), v16(320, 380), END(), COLOR_A(255),
              // above a polyline, scissored to a band so it does not cover the rest
              SCISSOR_XY(340, 280), SCISSOR_SIZE(220, 200), COLOR_RGB(220, 220, 60),
              BEGIN(EDGE_STRIP_A), v16(340, 430), v16(450, 380.3f), v16(560, 450), END(),
              SCISSOR_XY(0, 0), SCISSOR_SIZE(2048, 2048), COLOR_RGB(60, 220, 220), BEGIN(EDGE_STRIP_L), v16(60, 270),
              v16(30, 330), END() });
    return c.end();
}

std::shared_ptr<const Frame> sceneRectsScissor(Chip& c)
{
    c.begin();
    c.words({ CLEAR_COLOR_RGB(30, 30, 30), CLEAR(1, 1, 1),
              // radius 1 (default 16), 0, 4, 12 px
              BEGIN(RECTS), COLOR_RGB(200, 200, 200), v16(20, 20), v16(120, 60), LINE_WIDTH(0),
              COLOR_RGB(255, 120, 0), v16(140, 20), v16(240, 60), LINE_WIDTH(64),
              COLOR_RGB(0, 160, 255), v16(270.5f, 24.5f), v16(370, 56.25f), LINE_WIDTH(192),
              COLOR_RGB(255, 255, 255), v16(410, 30), v16(560, 110), END(),
              // translucent overlaps
              LINE_WIDTH(32), BEGIN(RECTS), COLOR_RGB(255, 0, 0), COLOR_A(140), v16(40, 100),
              v16(160, 180), COLOR_RGB(0, 255, 0), v16(100, 140), v16(220, 220), END(),
              COLOR_A(255),
              // (SRC_ALPHA, ZERO), as the core uses for its backgrounds
              BLEND_FUNC(SRC_ALPHA, ZERO), COLOR_RGB(90, 90, 240), BEGIN(RECTS), v16(600, 20),
              v16(780, 100), END(), BLEND_FUNC(SRC_ALPHA, ONE_MINUS_SRC_ALPHA),
              // scissor clips a big rounded rect and a thick line
              SCISSOR_XY(250, 150), SCISSOR_SIZE(200, 120), LINE_WIDTH(160),
              COLOR_RGB(240, 200, 40), BEGIN(RECTS), v16(200, 140), v16(330, 300), END(),
              LINE_WIDTH(96), COLOR_RGB(40, 200, 240), BEGIN(LINES), v16(260, 300), v16(470, 130),
              END(),
              // a scissored gradient and a scissored clear
              SCISSOR_XY(500, 150), SCISSOR_SIZE(280, 100), CMD_GRADIENT,
              (150u << 16) | 500u, 0xFF0000u, (250u << 16) | 780u, 0x0000FFu,
              SCISSOR_XY(560, 170), SCISSOR_SIZE(60, 40), CLEAR_COLOR_RGB(0, 120, 0),
              CLEAR(1, 1, 1),
              // nested context: the scissor is restored
              SAVE_CONTEXT(), SCISSOR_XY(20, 300), SCISSOR_SIZE(300, 60), COLOR_RGB(255, 0, 255),
              BEGIN(FTPOINTS), POINT_SIZE(800), v16(170, 330), END(), RESTORE_CONTEXT(),
              SCISSOR_XY(0, 0), SCISSOR_SIZE(2048, 2048), COLOR_RGB(255, 255, 255),
              LINE_WIDTH(16), BEGIN(RECTS), v16(20, 400), v16(780, 401), v16(20, 440),
              v16(21, 470), END() });
    return c.end();
}

std::shared_ptr<const Frame> sceneTextBitmaps(Chip& c)
{
    const std::vector<uint8_t> font = loadFont(c);
    // 8x8 L8 checker-ish gradient, a 4x4 RGB565, a 4x2 PALETTED4444 with its palette
    std::vector<uint8_t> l8(64);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            l8[size_t(y * 8 + x)] = uint8_t(((x + y) & 1) ? 255 : x * 32);
    c.m.writeDataRAMG(l8.data(), 64, 200000);
    std::vector<uint8_t> rgb(32);
    for (int i = 0; i < 16; i++)
    {
        const uint16_t v = uint16_t(((i * 2) << 11) | ((i * 4) << 5) | (31 - i * 2));
        rgb[size_t(i * 2)] = uint8_t(v);
        rgb[size_t(i * 2 + 1)] = uint8_t(v >> 8);
    }
    c.m.writeDataRAMG(rgb.data(), 32, 210000);
    const uint8_t idx[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
    const uint16_t pal[4] = { 0xFF00, 0xF0F0, 0x800F, 0xFFFF }; // ARGB4
    c.m.writeDataRAMG(idx, 8, 220000);
    c.m.writeDataRAMG(reinterpret_cast<const uint8_t*>(pal), 8, 230000);

    c.begin();
    c.words({ CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), COLOR_RGB(255, 255, 255) });
    c.text(20, 20, kFontHandle, 0, "Pattern 01  Track 1  C-4 08 ---");
    c.words({ COLOR_RGB(255, 180, 0) });
    c.text(400, 60, kFontHandle, OPT_CENTER, "Opening last project...");
    c.words({ COLOR_RGB(120, 200, 255), COLOR_A(150) });
    c.text(780, 90, kFontHandle, OPT_RIGHTX, "v1.0.0 CE (translucent)");
    c.words({ COLOR_A(255), COLOR_RGB(255, 255, 255) });
    c.text(700, 300, kFontHandle, 0, "x>511"); // CELL + VERTEX2F path
    verticalText(c, font, 40, 440, "Vertical label");
    c.words({ // L8 bitmap: native, scaled 4x nearest, scaled 4x bilinear, repeated
              BITMAP_HANDLE(7), BITMAP_SOURCE(200000), uint32_t(BITMAP_LAYOUT(L8, 8, 8)),
              uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 8, 8)), BEGIN(BITMAPS),
              COLOR_RGB(255, 255, 255), VERTEX2II(200, 150, 7, 0), END(), BITMAP_TRANSFORM_A(64),
              BITMAP_TRANSFORM_E(64), uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 32, 32)),
              BEGIN(BITMAPS), VERTEX2II(220, 150, 7, 0), END(),
              uint32_t(BITMAP_SIZE(BILINEAR, BORDER, BORDER, 40, 40)), COLOR_RGB(255, 200, 100),
              BEGIN(BITMAPS), VERTEX2II(260, 150, 7, 0), END(), BITMAP_TRANSFORM_A(256),
              BITMAP_TRANSFORM_E(256), uint32_t(BITMAP_SIZE(NEAREST, REPEAT, REPEAT, 60, 20)),
              COLOR_RGB(255, 255, 255), BEGIN(BITMAPS), VERTEX2II(320, 150, 7, 0), END(),
              // RGB565 4x4 scaled 8x
              BITMAP_HANDLE(8), BITMAP_SOURCE(210000), uint32_t(BITMAP_LAYOUT(RGB565, 8, 4)),
              BITMAP_TRANSFORM_A(32), BITMAP_TRANSFORM_E(32),
              uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 32, 32)), BEGIN(BITMAPS),
              VERTEX2II(400, 150, 8, 0), END(),
              // PALETTED4444 4x2 scaled 8x
              BITMAP_HANDLE(9), BITMAP_SOURCE(220000), uint32_t(BITMAP_LAYOUT(PALETTED4444, 4, 2)),
              PALETTE_SOURCE(230000), uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 32, 16)),
              BEGIN(BITMAPS), VERTEX2II(450, 150, 9, 0), END(), BITMAP_TRANSFORM_A(256),
              BITMAP_TRANSFORM_E(256) });
    // a small row of numbers like the tracker
    c.words({ COLOR_RGB(180, 255, 180), CMD_NUMBER, (220u << 16) | 500u, kFontHandle, 1234 });
    return c.end();
}

// ---- images ----

struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

bool writePng(const Image& img, const std::string& path)
{
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, img.rgb.data(), CFIndex(img.rgb.size()));
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGImageRef cg = CGImageCreate(size_t(img.w), size_t(img.h), 8, 24, size_t(img.w) * 3, cs,
                                  kCGImageAlphaNone, dp, nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst)
    {
        CGImageDestinationAddImage(dst, cg, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(cg);
    CGDataProviderRelease(dp);
    CFRelease(data);
    CGColorSpaceRelease(cs);
    return ok;
}

// Reads a PNG back as tightly packed 8-bit RGB (no colour conversion).
bool readPng(const std::string& path, Image& img)
{
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!src)
        return false;
    CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!cg)
        return false;
    img.w = int(CGImageGetWidth(cg));
    img.h = int(CGImageGetHeight(cg));
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(cg));
    const size_t bpr = CGImageGetBytesPerRow(cg), bpp = CGImageGetBitsPerPixel(cg) / 8;
    const uint8_t* p = CFDataGetBytePtr(data);
    img.rgb.resize(size_t(img.w) * img.h * 3);
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
            for (int k = 0; k < 3; k++)
                img.rgb[(size_t(y) * img.w + x) * 3 + k] = p[size_t(y) * bpr + size_t(x) * bpp + k];
    CFRelease(data);
    CGImageRelease(cg);
    return true;
}

Image cpuImage(const Frame& f, const Ft812Model& m) { return Image{ W, H, CpuRaster::render(f, m) }; }

// Fraction of pixels where any channel differs by more than `tol`.
double diffFraction(const Image& a, const Image& b, int tol, int* maxDiff = nullptr)
{
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    size_t bad = 0;
    int mx = 0;
    for (size_t i = 0; i < size_t(a.w) * a.h; i++)
    {
        int d = 0;
        for (int k = 0; k < 3; k++)
            d = std::max(d, std::abs(int(a.rgb[i * 3 + k]) - int(b.rgb[i * 3 + k])));
        mx = std::max(mx, d);
        bad += d > tol;
    }
    if (maxDiff)
        *maxDiff = mx;
    return double(bad) / double(size_t(a.w) * a.h);
}

// Averages s x s blocks (scaled render back to 800x480).
Image downsample(const Image& img, int s)
{
    Image out{ img.w / s, img.h / s, {} };
    out.rgb.resize(size_t(out.w) * out.h * 3);
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++)
            for (int k = 0; k < 3; k++)
            {
                int sum = 0;
                for (int j = 0; j < s; j++)
                    for (int i = 0; i < s; i++)
                        sum += img.rgb[(size_t(y * s + j) * img.w + size_t(x * s + i)) * 3 + k];
                out.rgb[(size_t(y) * out.w + x) * 3 + k] = uint8_t((sum + s * s / 2) / (s * s));
            }
    return out;
}

Image fromBGRA(const std::vector<uint8_t>& bgra, int w, int h)
{
    Image img{ w, h, std::vector<uint8_t>(size_t(w) * h * 3) };
    for (size_t i = 0; i < size_t(w) * h; i++)
    {
        img.rgb[i * 3 + 0] = bgra[i * 4 + 2];
        img.rgb[i * 3 + 1] = bgra[i * 4 + 1];
        img.rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    return img;
}

std::string outDir()
{
    mkdir(RENDER_OUT_DIR, 0755);
    return RENDER_OUT_DIR;
}

MetalRenderer& renderer()
{
    static std::unique_ptr<MetalRenderer> r = MetalRenderer::create();
    REQUIRE(r);
    REQUIRE(r->ok());
    return *r;
}

constexpr int kTol = 16;         // a channel difference you can see
constexpr double kMaxDiff = 0.01; // brief: <= 1 % of pixels against the golden

void checkGolden(const char* name, const Frame& f, const Ft812Model& m)
{
    const std::string golden = std::string(GOLDEN_DIR) + "/" + name + ".png";
    const Image cpu = cpuImage(f, m);
    Image g;
    if (!readPng(golden, g))
    {
        REQUIRE(writePng(cpu, golden));
        FAIL("golden " << golden << " did not exist: written from CpuRaster; inspect it and commit");
    }
    // the golden is CpuRaster's output (guards against either drifting silently)
    REQUIRE(diffFraction(cpu, g, 0) == 0.0);

    const std::string out = outDir() + "/" + name + "_metal.png";
    REQUIRE(renderer().renderToPNG(f, out.c_str()));
    Image metal;
    REQUIRE(readPng(out, metal));
    int maxDiff = 0;
    const double d = diffFraction(metal, g, kTol, &maxDiff);
    const double any = diffFraction(metal, g, 2);
    INFO(name << ": " << d * 100 << " % of pixels differ by > " << kTol << " (" << any * 100
              << " % by > 2, max " << maxDiff << "); metal: " << out);
    CHECK(d <= kMaxDiff);
    std::printf("%s: %.4f %% > %d, %.4f %% > 2, max channel diff %d\n", name, d * 100, kTol,
                any * 100, maxDiff);
}
} // namespace

TEST_CASE("golden: lines and points")
{
    Chip c;
    auto f = sceneLinesPoints(c);
    REQUIRE(f);
    checkGolden("lines_points", *f, c.m);
}

TEST_CASE("golden: rounded rects and scissor")
{
    Chip c;
    auto f = sceneRectsScissor(c);
    REQUIRE(f);
    checkGolden("rects_scissor", *f, c.m);
}

TEST_CASE("golden: text and bitmaps")
{
    Chip c;
    auto f = sceneTextBitmaps(c);
    REQUIRE(f);
    checkGolden("text_bitmaps", *f, c.m);
}

TEST_CASE("viewport: integer scale when it fits, else uniform; centred")
{
    auto v = MetalRenderer::fit(CGSizeMake(800, 480));
    CHECK((v.scale == 1 && v.x == 0 && v.y == 0));
    v = MetalRenderer::fit(CGSizeMake(1600, 960));
    CHECK((v.scale == 2 && v.x == 0 && v.y == 0));
    v = MetalRenderer::fit(CGSizeMake(1700, 1000));
    CHECK((v.scale == 2 && v.x == 50 && v.y == 20));
    v = MetalRenderer::fit(CGSizeMake(2560, 1600)); // 3x would need 1440 rows: 3x fits, 2400x1440
    CHECK((v.scale == 3 && v.x == 80 && v.y == 80));
    v = MetalRenderer::fit(CGSizeMake(640, 480)); // too small: 0.8, letterboxed vertically
    CHECK(std::fabs(v.scale - 0.8f) < 1e-6f);
    CHECK((v.x == 0 && v.y == 48));
}

TEST_CASE("scaled rendering matches the golden after downsampling")
{
    Chip c;
    auto f = sceneRectsScissor(c);
    std::vector<uint8_t> bgra;
    REQUIRE(renderer().renderToBGRA(*f, 2, bgra));
    REQUIRE(bgra.size() == size_t(W * 2) * H * 2 * 4);
    const Image big = fromBGRA(bgra, W * 2, H * 2);
    writePng(big, outDir() + "/rects_scissor_metal_2x.png");
    const double d = diffFraction(downsample(big, 2), cpuImage(*f, c.m), kTol);
    INFO(d * 100 << " % differ");
    CHECK(d <= 0.02);
}

TEST_CASE("draw() letterboxes into a larger drawable")
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    REQUIRE(dev);
    MetalRenderer r(dev);
    REQUIRE(r.ok());
    Chip c;
    auto f = c.frame({ CLEAR_COLOR_RGB(0, 0, 255), CLEAR(1, 1, 1), COLOR_RGB(255, 0, 0),
                       LINE_WIDTH(0), BEGIN(RECTS), VERTEX2II(0, 0, 0, 0), VERTEX2II(10, 10, 0, 0),
                       END() });
    const int tw = 1700, th = 1000; // 2x, offset (50, 20)
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:tw
                                                                                 height:th
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1); // letterbox: green
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    r.draw(*f, enc, CGSizeMake(tw, th));
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<uint8_t> px(size_t(tw) * th * 4);
    [tex getBytes:px.data() bytesPerRow:size_t(tw) * 4 fromRegion:MTLRegionMake2D(0, 0, tw, th) mipmapLevel:0];
    auto at = [&](int x, int y) {
        const uint8_t* p = &px[(size_t(y) * tw + x) * 4];
        return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
    };
    CHECK(at(10, 10) == 0x00FF00);     // letterbox untouched
    CHECK(at(49, 500) == 0x00FF00);
    CHECK(at(1650, 500) == 0x00FF00);
    CHECK(at(50, 20) == 0xFF0000);     // logical (0, 0)
    CHECK(at(69, 39) == 0xFF0000);     // logical (9.75, 9.75)
    CHECK(at(70, 40) == 0x0000FF);     // logical (10, 10): clear colour
    CHECK(at(1649, 979) == 0x0000FF);  // logical (799.75, 479.75)
}

TEST_CASE("bitmap texels are cached by address and generation; frames keep their own bytes")
{
    MetalRenderer& r = renderer();
    Chip c;
    std::vector<uint8_t> img(16, 255); // 4x4 L8, white
    c.m.writeDataRAMG(img.data(), 16, 300000);
    const std::initializer_list<uint32_t> list = {
        CLEAR_COLOR_RGB(0, 0, 0), CLEAR(1, 1, 1), BITMAP_HANDLE(3), BITMAP_SOURCE(300000),
        uint32_t(BITMAP_LAYOUT(L8, 4, 4)), uint32_t(BITMAP_SIZE(NEAREST, BORDER, BORDER, 4, 4)),
        COLOR_RGB(0, 255, 0), BEGIN(BITMAPS), VERTEX2II(10, 10, 3, 0), END()
    };
    auto f1 = c.frame(list);
    const uint64_t d0 = r.bitmapDecodes();
    r.uploadBitmaps(*f1);
    CHECK(r.bitmapDecodes() == d0 + 1);
    r.uploadBitmaps(c.m); // same frame, same bytes: cached
    auto f2 = c.frame(list);
    r.uploadBitmaps(*f2); // a new frame over unchanged RAM_G: cached
    CHECK(r.bitmapDecodes() == d0 + 1);

    std::vector<uint8_t> bgra;
    auto green = [&](const Frame& f) {
        REQUIRE(r.renderToBGRA(f, 1, bgra));
        return bgra[(size_t(11) * W + 11) * 4 + 1];
    };
    CHECK(green(*f2) == 255);
    std::vector<uint8_t> dark(16, 64);
    c.m.writeDataRAMG(dark.data(), 16, 300000); // the tracker thread rewrites the bitmap
    CHECK(green(*f2) == 255);                   // the published frame still shows its own bytes
    auto f3 = c.frame(list);
    CHECK(green(*f3) == 64); // the next frame sees the new texels (new generation)
    CHECK(r.bitmapDecodes() == d0 + 2);
}

TEST_CASE("snapshot hook renders the latest frame as ARGB8 into ram_g")
{
    MetalRenderer& r = renderer();
    Chip c;
    r.attachSnapshotHook(c.m);
    c.frame({ CLEAR_COLOR_RGB(0, 0, 255), CLEAR(1, 1, 1), COLOR_RGB(255, 0, 0), LINE_WIDTH(0),
              BEGIN(RECTS), VERTEX2II(0, 240, 0, 0), VERTEX2II(4, 244, 0, 0), END() });
    c.m.beginCoProList();
    // the core's call: ARGB8 (0x20), bottom half of the screen, width 1600 = 800 px
    c.words({ CMD_SNAPSHOT2, 0x20, 150000, (240u << 16) | 0u, (240u << 16) | 1600u });
    c.m.endCoProList();
    const uint8_t* p = c.m.ramG() + 150000;
    CHECK((p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255));               // red, B G R A
    CHECK((p[4 * 4 + 0] == 255 && p[4 * 4 + 2] == 0 && p[4 * 4 + 3] == 255));     // blue
    const uint8_t* last = p + (size_t(239) * 800 + 799) * 4;
    CHECK((last[0] == 255 && last[1] == 0 && last[2] == 0 && last[3] == 255));
    // a rect reaching past the screen edge: outside pixels are opaque black
    c.m.beginCoProList();
    c.words({ CMD_SNAPSHOT2, 0x20, 950000, (0u << 16) | 798u, (1u << 16) | 8u });
    c.m.endCoProList();
    const uint8_t* q = c.m.ramG() + 950000;
    CHECK((q[0] == 255 && q[3] == 255));                         // x 798: blue
    CHECK((q[8] == 0 && q[9] == 0 && q[10] == 0 && q[11] == 255)); // x 800: black
    MetalRenderer::detachSnapshotHook(c.m);
    CHECK(!c.m.snapshotHook);
}
