#include "CpuRaster.h"

#include "FT81x.h"
#include "Ft812Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
using ft812::kScreenHeight;
using ft812::kScreenWidth;

struct Rgba
{
    float r, g, b, a;
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
        int x0 = std::max({ int(std::floor(fx0)), p.scissor.x, 0 });
        int y0 = std::max({ int(std::floor(fy0)), p.scissor.y, 0 });
        int x1 = std::min({ int(std::ceil(fx1)), p.scissor.x + p.scissor.w, kScreenWidth });
        int y1 = std::min({ int(std::ceil(fy1)), p.scissor.y + p.scissor.h, kScreenHeight });
        const Rgba col = unpack(p.rgba);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                const float c = cov(float(x) + 0.5f, float(y) + 0.5f);
                if (c <= 0)
                    continue;
                Rgba& d = px_[size_t(y) * kScreenWidth + x];
                const Rgba s{ col.r, col.g, col.b, col.a * c * alphaScale };
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
    const float r = p.lineWidthPx;
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

void drawPrim(Canvas& cv, const Prim& p)
{
    const auto& v = p.verts;
    switch (p.kind)
    {
        case Prim::Points:
            for (const Vec2& c : v)
            {
                const float r = p.pointSizePx;
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
                const float r = p.lineWidthPx;
                cv.draw(p, x0 - r - 1, y0 - r - 1, x1 + r + 1, y1 + r + 1, [&](float x, float y) {
                    return coverage(boxDistance(x, y, x0, y0, x1, y1), r);
                });
            }
            break;
        case Prim::Bitmap:
        {
            // Placeholder until Task 5b samples texels: the cell box, translucent.
            const Vec2 o = v[0];
            const float w = float(std::min<uint32_t>(p.w, 2048));
            const float h = float(std::min<uint32_t>(p.h, 2048));
            auto inside = [&](float x, float y) {
                return (x >= o.x && x < o.x + w && y >= o.y && y < o.y + h) ? 1.0f : 0.0f;
            };
            cv.draw(p, o.x, o.y, o.x + w, o.y + h, inside, 0.35f);
            break;
        }
    }
}
} // namespace

std::vector<uint8_t> CpuRaster::render(const Frame& frame, const Ft812Model& model)
{
    (void)model; // texels (Task 5b)
    Canvas cv(frame.clearColor);
    for (const Prim& p : frame.prims)
        drawPrim(cv, p);
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
