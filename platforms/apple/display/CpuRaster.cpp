#include "CpuRaster.h"

#include "Bitmaps.h"
#include "FT81x.h"
#include "Ft812Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

namespace
{
using ft812::kScreenHeight;
using ft812::kScreenWidth;

struct Rgba
{
    float r, g, b, a;
};

// Source colour of one pixel and how much of the pixel the primitive covers (0 = untouched).
struct Sample
{
    Rgba c;
    float cov;
};

Rgba unpack(uint32_t c)
{
    return { float((c >> 24) & 255) / 255.0f, float((c >> 16) & 255) / 255.0f,
             float((c >> 8) & 255) / 255.0f, float(c & 255) / 255.0f };
}

float factor(uint8_t f, const Rgba& src, const Rgba& dst)
{
    switch (f)
    {
        case ZERO: return 0.0f;
        case ONE: return 1.0f;
        case SRC_ALPHA: return src.a;
        case DST_ALPHA: return dst.a;
        case ONE_MINUS_SRC_ALPHA: return 1.0f - src.a;
        case ONE_MINUS_DST_ALPHA: return 1.0f - dst.a;
        default: return 0.0f;
    }
}

// Antialiased coverage of a pixel whose centre is `sd` px (signed distance, negative inside)
// from the primitive's core shape, grown by radius r.
float coverage(float sd, float r) { return std::clamp(r - sd + 0.5f, 0.0f, 1.0f); }

float segmentDistance(float px, float py, Vec2 a, Vec2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = px - (a.x + t * dx), ey = py - (a.y + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

// Signed distance from a point to an axis-aligned box (negative inside).
float boxDistance(float px, float py, float x0, float y0, float x1, float y1)
{
    const float dx = std::max(x0 - px, px - x1), dy = std::max(y0 - py, py - y1);
    if (dx <= 0 && dy <= 0)
        return std::max(dx, dy);
    const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    return std::sqrt(ox * ox + oy * oy);
}

class Canvas
{
  public:
    explicit Canvas(uint32_t clear) : px_(size_t(kScreenWidth) * kScreenHeight, unpack(clear)) {}

    // Draws one element of `p`: cov(x + 0.5, y + 0.5) for pixels in [x0,x1) x [y0,y1), clipped to
    // the prim's scissor and the screen.
    template <typename Cov>
    void draw(const Prim& p, float fx0, float fy0, float fx1, float fy1, Cov cov,
              float alphaScale = 1)
    {
        const Rgba col = unpack(p.rgba);
        shade(p, fx0, fy0, fx1, fy1, [&](float x, float y) {
            return Sample{ col, cov(x, y) * alphaScale };
        });
    }

    // Like draw(), but `src(x, y)` gives the source colour per pixel centre with its coverage,
    // which multiplies the source alpha; coverage <= 0 leaves the pixel alone.
    template <typename Src>
    void shade(const Prim& p, float fx0, float fy0, float fx1, float fy1, Src src)
    {
        int x0 = std::max({ int(std::floor(fx0)), p.scissor.x, 0 });
        int y0 = std::max({ int(std::floor(fy0)), p.scissor.y, 0 });
        int x1 = std::min({ int(std::ceil(fx1)), p.scissor.x + p.scissor.w, kScreenWidth });
        int y1 = std::min({ int(std::ceil(fy1)), p.scissor.y + p.scissor.h, kScreenHeight });
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                const Sample sm = src(float(x) + 0.5f, float(y) + 0.5f);
                if (sm.cov <= 0)
                    continue;
                const Rgba s{ sm.c.r, sm.c.g, sm.c.b, sm.c.a * sm.cov };
                Rgba& d = px_[size_t(y) * kScreenWidth + x];
                const float fs = factor(p.blend.src, s, d), fd = factor(p.blend.dst, s, d);
                d = Rgba{ std::clamp(s.r * fs + d.r * fd, 0.0f, 1.0f),
                          std::clamp(s.g * fs + d.g * fd, 0.0f, 1.0f),
                          std::clamp(s.b * fs + d.b * fd, 0.0f, 1.0f),
                          std::clamp(s.a * fs + d.a * fd, 0.0f, 1.0f) };
            }
    }

    std::vector<uint8_t> rgb() const
    {
        std::vector<uint8_t> out(px_.size() * 3);
        for (size_t i = 0; i < px_.size(); i++)
        {
            out[i * 3 + 0] = uint8_t(std::lround(px_[i].r * 255.0f));
            out[i * 3 + 1] = uint8_t(std::lround(px_[i].g * 255.0f));
            out[i * 3 + 2] = uint8_t(std::lround(px_[i].b * 255.0f));
        }
        return out;
    }

  private:
    std::vector<Rgba> px_;
};

void drawSegment(Canvas& cv, const Prim& p, Vec2 a, Vec2 b)
{
    const float r = p.lineRadiusPx;
    cv.draw(p, std::min(a.x, b.x) - r - 1, std::min(a.y, b.y) - r - 1, std::max(a.x, b.x) + r + 1,
            std::max(a.y, b.y) + r + 1,
            [&](float x, float y) { return coverage(segmentDistance(x, y, a, b), r); });
}

// Edge strip segment: fills from the segment to the screen edge on the prim's side, for pixel
// centres inside the segment's span (half-open, so joined segments do not overlap).
void drawEdge(Canvas& cv, const Prim& p, Vec2 a, Vec2 b)
{
    const bool horizontalFill = p.edge == Prim::EdgeR || p.edge == Prim::EdgeL;
    if (horizontalFill)
    {
        if (a.y == b.y)
            return;
        if (a.y > b.y)
            std::swap(a, b);
        const float x0 = p.edge == Prim::EdgeR ? std::min(a.x, b.x) - 1 : 0;
        const float x1 = p.edge == Prim::EdgeR ? float(kScreenWidth) : std::max(a.x, b.x) + 1;
        cv.draw(p, x0, a.y - 1, x1, b.y + 1, [&](float x, float y) {
            if (y < a.y || y >= b.y)
                return 0.0f;
            const float lx = a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y);
            const float d = p.edge == Prim::EdgeR ? x - lx : lx - x;
            return std::clamp(d + 0.5f, 0.0f, 1.0f);
        });
    }
    else
    {
        if (a.x == b.x)
            return;
        if (a.x > b.x)
            std::swap(a, b);
        const float y0 = p.edge == Prim::EdgeB ? std::min(a.y, b.y) - 1 : 0;
        const float y1 = p.edge == Prim::EdgeB ? float(kScreenHeight) : std::max(a.y, b.y) + 1;
        cv.draw(p, a.x - 1, y0, b.x + 1, y1, [&](float x, float y) {
            if (x < a.x || x >= b.x)
                return 0.0f;
            const float ly = a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
            const float d = p.edge == Prim::EdgeB ? y - ly : ly - y;
            return std::clamp(d + 0.5f, 0.0f, 1.0f);
        });
    }
}

// Decoded bitmap cells of one render, keyed by (address, format, stride, rows, palette).
using TexelKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
using TexelCache = std::map<TexelKey, std::vector<uint32_t>>;

void drawBitmap(Canvas& cv, const Prim& p, const Ft812Model& model, TexelCache& cache)
{
    const uint32_t lw = Bitmaps::layoutWidth(p.fmt, p.stride), lh = p.layoutH;
    if (lw == 0 || lh == 0)
        return;
    // Cell n starts n * stride * height bytes after the source; 22-bit address arithmetic
    // (CMD_SETFONT2 sources point before the first glyph).
    const uint32_t cellBytes = p.stride * lh;
    const uint32_t addr = (p.srcAddr + p.cell * cellBytes) & 0x3FFFFF;
    if (uint64_t(addr) + cellBytes > Ft812Model::kRamGSize)
        return;
    const uint32_t palBytes = Bitmaps::paletteBytes(p.fmt);
    const bool palOk = palBytes && uint64_t(p.paletteAddr) + palBytes <= Ft812Model::kRamGSize;
    const TexelKey key{ addr, p.fmt, p.stride, lh, palBytes ? p.paletteAddr : 0 };
    auto it = cache.find(key);
    if (it == cache.end())
        it = cache
               .emplace(key, Bitmaps::decode(p.fmt, model.ramG() + addr, lw, lh, p.stride,
                                             palOk ? model.ramG() + p.paletteAddr : nullptr))
               .first;
    const std::vector<uint32_t>& tex = it->second;

    // BORDER: outside the layout is transparent black; REPEAT wraps.
    auto texel = [&](int tx, int ty) -> Rgba {
        if (p.wrapX == REPEAT)
            tx = ((tx % int(lw)) + int(lw)) % int(lw);
        if (p.wrapY == REPEAT)
            ty = ((ty % int(lh)) + int(lh)) % int(lh);
        if (tx < 0 || ty < 0 || tx >= int(lw) || ty >= int(lh))
            return Rgba{ 0, 0, 0, 0 };
        return unpack(tex[size_t(ty) * lw + size_t(tx)]);
    };

    const Vec2 o = p.verts[0];
    const float w = float(p.w), h = float(p.h);
    const float* m = p.xform;
    const Rgba col = unpack(p.rgba);
    cv.shade(p, o.x, o.y, o.x + w, o.y + h, [&](float x, float y) {
        const float sx = x - o.x, sy = y - o.y;
        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
            return Sample{ {}, 0.0f };
        const float u = m[0] * sx + m[1] * sy + m[2], v = m[3] * sx + m[4] * sy + m[5];
        Rgba t;
        if (p.filter == BILINEAR)
        {
            const float fu = u - 0.5f, fv = v - 0.5f;
            const int iu = int(std::floor(fu)), iv = int(std::floor(fv));
            const float au = fu - float(iu), av = fv - float(iv);
            const Rgba t00 = texel(iu, iv), t10 = texel(iu + 1, iv);
            const Rgba t01 = texel(iu, iv + 1), t11 = texel(iu + 1, iv + 1);
            auto mix = [&](float a, float b, float c, float d) {
                return (a * (1 - au) + b * au) * (1 - av) + (c * (1 - au) + d * au) * av;
            };
            t = Rgba{ mix(t00.r, t10.r, t01.r, t11.r), mix(t00.g, t10.g, t01.g, t11.g),
                      mix(t00.b, t10.b, t01.b, t11.b), mix(t00.a, t10.a, t01.a, t11.a) };
        }
        else
            t = texel(int(std::floor(u)), int(std::floor(v)));
        // texel colour modulated by COLOR_RGB / COLOR_A
        return Sample{ { t.r * col.r, t.g * col.g, t.b * col.b, t.a * col.a }, 1.0f };
    });
}

void drawPrim(Canvas& cv, const Prim& p, const Ft812Model& model, TexelCache& cache)
{
    const auto& v = p.verts;
    switch (p.kind)
    {
        case Prim::Points:
            for (const Vec2& c : v)
            {
                const float r = p.pointRadiusPx;
                auto disc = [&](float x, float y) {
                    return coverage(std::hypot(x - c.x, y - c.y), r);
                };
                cv.draw(p, c.x - r - 1, c.y - r - 1, c.x + r + 1, c.y + r + 1, disc);
            }
            break;
        case Prim::Lines:
            for (size_t i = 0; i + 1 < v.size(); i += 2)
                drawSegment(cv, p, v[i], v[i + 1]);
            break;
        case Prim::LineStrip:
            for (size_t i = 0; i + 1 < v.size(); i++)
                drawSegment(cv, p, v[i], v[i + 1]);
            break;
        case Prim::EdgeStrip:
            for (size_t i = 0; i + 1 < v.size(); i++)
                drawEdge(cv, p, v[i], v[i + 1]);
            break;
        case Prim::Rects:
            for (size_t i = 0; i + 1 < v.size(); i += 2)
            {
                const float x0 = std::min(v[i].x, v[i + 1].x), x1 = std::max(v[i].x, v[i + 1].x);
                const float y0 = std::min(v[i].y, v[i + 1].y), y1 = std::max(v[i].y, v[i + 1].y);
                const float r = p.lineRadiusPx;
                cv.draw(p, x0 - r - 1, y0 - r - 1, x1 + r + 1, y1 + r + 1, [&](float x, float y) {
                    return coverage(boxDistance(x, y, x0, y0, x1, y1), r);
                });
            }
            break;
        case Prim::Bitmap: drawBitmap(cv, p, model, cache); break;
        case Prim::Gradient:
        {
            const Vec2 a = v[0], b = v[1];
            const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            const Rgba c0 = unpack(p.rgba), c1 = unpack(p.rgba2);
            // Fills the scissor: colour by the projection onto p0 -> p1, clamped at the ends.
            cv.shade(p, 0, 0, float(kScreenWidth), float(kScreenHeight), [&](float x, float y) {
                float t = len2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                return Sample{ { c0.r + (c1.r - c0.r) * t, c0.g + (c1.g - c0.g) * t,
                                 c0.b + (c1.b - c0.b) * t, c0.a + (c1.a - c0.a) * t },
                               1.0f };
            });
            break;
        }
    }
}
} // namespace

std::vector<uint8_t> CpuRaster::render(const Frame& frame, const Ft812Model& model)
{
    Canvas cv(frame.clearColor);
    TexelCache cache;
    for (const Prim& p : frame.prims)
        drawPrim(cv, p, model, cache);
    return cv.rgb();
}

bool CpuRaster::toPPM(const Frame& frame, const Ft812Model& model, const char* path)
{
    const std::vector<uint8_t> rgb = render(frame, model);
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", kScreenWidth, kScreenHeight);
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    return std::fclose(f) == 0 && ok;
}
