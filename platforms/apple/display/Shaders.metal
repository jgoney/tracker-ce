// FT812 primitives as instanced quads (see MetalRenderer.h). Every element evaluates the same
// distances and coverage as CpuRaster.cpp, in logical (800x480) pixels; `scale` device pixels per
// logical pixel sets the feathering to one device pixel. Embedded into the binary by CMake and
// compiled at runtime (no dependency on the optional Metal toolchain).
#include <metal_stdlib>
using namespace metal;

// Must match MetalRenderer.mm (Inst).
enum Kind : uint
{
    KindDisc = 0,     // a.xy centre, r radius
    KindSegment = 1,  // a.xy - a.zw capsule, r radius
    KindBox = 2,      // a = x0, y0, x1, y1, grown by r with round corners
    KindEdge = 3,     // a.xy - a.zw, k.y = edge (0 R, 1 L, 2 A, 3 B)
    KindBitmap = 4,   // a.xy origin, m0 = A B C D, m1 = E F w h, k = (_, filter, texel base, lw),
                      // k2 = (lh, wrapX, wrapY, _)
    KindGradient = 5  // a.xy p0 (color), a.zw p1 (color2)
};

struct Inst
{
    float4 box;    // quad in logical px: x0, y0, x1, y1
    float4 a;
    float4 color;  // straight RGBA
    float4 color2;
    float4 m0;
    float4 m1;
    uint4 k;       // kind, ...
    uint4 k2;
    float4 r;      // radius
};

struct Uniforms
{
    float2 viewport; // drawable size, device px
    float2 offset;   // device px of logical (0, 0)
    float scale;
};

struct VOut
{
    float4 pos [[position]];
    uint inst [[flat]];
};

vertex VOut vs_main(uint vid [[vertex_id]], uint iid [[instance_id]],
                    const device Inst* insts [[buffer(0)]], constant Uniforms& u [[buffer(1)]])
{
    const Inst in = insts[iid];
    const float2 corner = float2((vid & 1) ? in.box.z : in.box.x, (vid & 2) ? in.box.w : in.box.y);
    const float2 px = corner * u.scale + u.offset;
    VOut o;
    o.pos = float4(px.x / u.viewport.x * 2 - 1, 1 - px.y / u.viewport.y * 2, 0, 1);
    o.inst = iid;
    return o;
}

static float segmentDistance(float2 p, float2 a, float2 b)
{
    const float2 d = b - a;
    const float len2 = dot(d, d);
    const float t = len2 > 0 ? clamp(dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + t * d));
}

static float boxDistance(float2 p, float4 b)
{
    const float dx = max(b.x - p.x, p.x - b.z), dy = max(b.y - p.y, p.y - b.w);
    if (dx <= 0 && dy <= 0)
        return max(dx, dy);
    return length(float2(max(dx, 0.0f), max(dy, 0.0f)));
}

static float4 unpackRgba(uint c)
{
    return float4(float((c >> 24) & 255), float((c >> 16) & 255), float((c >> 8) & 255),
                  float(c & 255)) / 255.0f;
}

// BORDER: transparent black outside the layout; REPEAT (1) wraps.
static float4 texel(const device uint* tex, const Inst in, int tx, int ty)
{
    const int lw = int(in.k.w), lh = int(in.k2.x);
    if (in.k2.y == 1)
        tx = ((tx % lw) + lw) % lw;
    if (in.k2.z == 1)
        ty = ((ty % lh) + lh) % lh;
    if (tx < 0 || ty < 0 || tx >= lw || ty >= lh)
        return float4(0);
    return unpackRgba(tex[in.k.z + uint(ty) * uint(lw) + uint(tx)]);
}

fragment float4 fs_main(VOut v [[stage_in]], const device Inst* insts [[buffer(0)]],
                        constant Uniforms& u [[buffer(1)]], const device uint* tex [[buffer(2)]])
{
    const Inst in = insts[v.inst];
    const float2 p = (v.pos.xy - u.offset) / u.scale; // pixel centre, logical px
    const float s = u.scale, r = in.r.x;
    float4 c = in.color;
    float cov = 1;
    switch (in.k.x)
    {
        case KindDisc: cov = clamp((r - distance(p, in.a.xy)) * s + 0.5f, 0.0f, 1.0f); break;
        case KindSegment:
            cov = clamp((r - segmentDistance(p, in.a.xy, in.a.zw)) * s + 0.5f, 0.0f, 1.0f);
            break;
        case KindBox: cov = clamp((r - boxDistance(p, in.a)) * s + 0.5f, 0.0f, 1.0f); break;
        case KindEdge:
        {
            const float2 a = in.a.xy, b = in.a.zw; // sorted along the span axis
            const uint e = in.k.y;
            if (e <= 1)
            {
                if (p.y < a.y || p.y >= b.y)
                    discard_fragment();
                const float lx = a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y);
                cov = clamp((e == 0 ? p.x - lx : lx - p.x) * s + 0.5f, 0.0f, 1.0f);
            }
            else
            {
                if (p.x < a.x || p.x >= b.x)
                    discard_fragment();
                const float ly = a.y + (b.y - a.y) * (p.x - a.x) / (b.x - a.x);
                cov = clamp((e == 3 ? p.y - ly : ly - p.y) * s + 0.5f, 0.0f, 1.0f);
            }
            break;
        }
        case KindBitmap:
        {
            const float sx = p.x - in.a.x, sy = p.y - in.a.y;
            if (sx < 0 || sy < 0 || sx >= in.m1.z || sy >= in.m1.w)
                discard_fragment();
            const float tu = in.m0.x * sx + in.m0.y * sy + in.m0.z;
            const float tv = in.m0.w * sx + in.m1.x * sy + in.m1.y;
            float4 t;
            if (in.k.y == 1) // BILINEAR
            {
                const float fu = tu - 0.5f, fv = tv - 0.5f;
                const int iu = int(floor(fu)), iv = int(floor(fv));
                const float au = fu - float(iu), av = fv - float(iv);
                t = mix(mix(texel(tex, in, iu, iv), texel(tex, in, iu + 1, iv), au),
                        mix(texel(tex, in, iu, iv + 1), texel(tex, in, iu + 1, iv + 1), au), av);
            }
            else
                t = texel(tex, in, int(floor(tu)), int(floor(tv)));
            c = t * in.color; // texel colour modulated by COLOR_RGB / COLOR_A
            break;
        }
        case KindGradient:
        {
            const float2 a = in.a.xy, d = in.a.zw - in.a.xy;
            const float len2 = dot(d, d);
            const float t = len2 > 0 ? clamp(dot(p - a, d) / len2, 0.0f, 1.0f) : 0.0f;
            c = mix(in.color, in.color2, t);
            break;
        }
    }
    if (cov <= 0)
        discard_fragment(); // CpuRaster leaves uncovered pixels alone, whatever the blend
    return float4(c.rgb, c.a * cov);
}
