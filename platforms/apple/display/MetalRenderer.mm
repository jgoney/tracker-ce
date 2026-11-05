#include "MetalRenderer.h"

#include "Bitmaps.h"
#include "FT81x.h"
#include "Ft812Model.h"
#include "ShadersSource.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

namespace
{
using ft812::kScreenHeight;
using ft812::kScreenWidth;

enum Kind : uint32_t
{
    KindDisc = 0,
    KindSegment = 1,
    KindBox = 2,
    KindEdge = 3,
    KindBitmap = 4,
    KindGradient = 5
};

// One element (one quad). Layout matches `Inst` in Shaders.metal: nine 16-byte vectors.
struct alignas(16) Inst
{
    float box[4];
    float a[4];
    float color[4];
    float color2[4];
    float m0[4];
    float m1[4];
    uint32_t k[4];
    uint32_t k2[4];
    float r[4];
};
static_assert(sizeof(Inst) == 144, "Inst layout must match Shaders.metal");

struct Uniforms
{
    float viewport[2];
    float offset[2];
    float scale;
    float pad[3];
};

// Consecutive elements sharing a pipeline and a scissor: one instanced draw.
struct Batch
{
    uint32_t first = 0, count = 0;
    BlendMode blend;
    PixelRect scissor; // logical, clipped to the screen
};

// GPU texel cache key: (cell address, format, stride, rows, palette address, generation).
using TexKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;

constexpr size_t kInitialTexels = 256 * 1024;   // 1 MB
constexpr size_t kMaxTexels = 16 * 1024 * 1024; // 64 MB, then the cache starts over

void setColor(float* out, uint32_t rgba)
{
    out[0] = float((rgba >> 24) & 255) / 255.0f;
    out[1] = float((rgba >> 16) & 255) / 255.0f;
    out[2] = float((rgba >> 8) & 255) / 255.0f;
    out[3] = float(rgba & 255) / 255.0f;
}

MTLBlendFactor blendFactor(uint8_t f)
{
    switch (f)
    {
        case ONE: return MTLBlendFactorOne;
        case SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        default: return MTLBlendFactorZero; // ZERO and invalid factors (CpuRaster: 0)
    }
}

PixelRect clipToScreen(const PixelRect& s)
{
    const int x0 = std::max(s.x, 0), y0 = std::max(s.y, 0);
    const int x1 = std::min(s.x + s.w, kScreenWidth), y1 = std::min(s.y + s.h, kScreenHeight);
    return PixelRect{ x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0) };
}
} // namespace

struct MetalRenderer::Impl
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    MTLPixelFormat colorFormat = MTLPixelFormatBGRA8Unorm;
    std::map<uint32_t, id<MTLRenderPipelineState>> pipelines; // (format << 16) | (src << 8) | dst
    bool ok = false;

    std::mutex mutex; // everything below, and encoding

    // Texel pool: decoded cells (RGBA8 as 0xRRGGBBAA) appended into one shared buffer the
    // fragment shader indexes, so bitmaps of any format/size batch like the other prims.
    id<MTLBuffer> pool;
    size_t poolUsed = 0, poolCap = 0; // texels
    std::map<TexKey, uint32_t> texels; // -> first texel in the pool
    uint64_t decodes = 0;

    std::vector<Inst> insts; // scratch
    std::vector<Batch> batches;

    bool init(id<MTLDevice> dev, MTLPixelFormat fmt)
    {
        device = dev;
        colorFormat = fmt;
        if (!device)
            return false;
        queue = [device newCommandQueue];
        NSError* err = nil;
        library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                       options:nil
                                         error:&err];
        if (!library)
        {
            NSLog(@"MetalRenderer: shader compilation failed: %@", err);
            return false;
        }
        poolCap = kInitialTexels;
        pool = [device newBufferWithLength:poolCap * 4 options:MTLResourceStorageModeShared];
        ok = queue && pool && pipeline(colorFormat, BlendMode{}) != nil;
        return ok;
    }

    id<MTLRenderPipelineState> pipeline(MTLPixelFormat fmt, BlendMode b)
    {
        const uint32_t key = (uint32_t(fmt) << 16) | (uint32_t(b.src) << 8) | b.dst;
        auto it = pipelines.find(key);
        if (it != pipelines.end())
            return it->second;
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:@"vs_main"];
        d.fragmentFunction = [library newFunctionWithName:@"fs_main"];
        MTLRenderPipelineColorAttachmentDescriptor* ca = d.colorAttachments[0];
        ca.pixelFormat = fmt;
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        // EVE applies the same factors to all four channels (CpuRaster too).
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = blendFactor(b.src);
        ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = blendFactor(b.dst);
        NSError* err = nil;
        id<MTLRenderPipelineState> ps = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!ps)
            NSLog(@"MetalRenderer: pipeline creation failed: %@", err);
        pipelines[key] = ps;
        return ps;
    }

    // Texels of a Bitmap prim's cell: key and the snapshot bytes, or false if nothing is drawn.
    struct CellRef
    {
        TexKey key;
        const uint8_t* src = nullptr;
        const uint8_t* palette = nullptr;
        uint32_t lw = 0, lh = 0;
    };
    static bool cellRef(const Frame& f, const Prim& p, CellRef& ref)
    {
        uint32_t addr = 0, bytes = 0, gen = 0, palGen = 0;
        if (!Bitmaps::cellRange(p, addr, bytes))
            return false;
        ref.src = f.ramGBytes(addr, bytes, &gen);
        if (!ref.src)
            return false;
        const uint32_t palBytes = Bitmaps::paletteBytes(p.fmt);
        if (palBytes)
            ref.palette = f.ramGBytes(p.paletteAddr, palBytes, &palGen);
        ref.lw = Bitmaps::layoutWidth(p.fmt, p.stride);
        ref.lh = p.layoutH;
        ref.key = TexKey{ addr, p.fmt, p.stride, p.layoutH, palBytes ? p.paletteAddr : 0,
                          std::max(gen, palGen) };
        return true;
    }

    void uploadLocked(const Frame& f)
    {
        std::map<TexKey, CellRef> missing;
        size_t missingTexels = 0, frameTexels = 0;
        std::map<TexKey, CellRef> all;
        for (const Prim& p : f.prims)
        {
            CellRef ref;
            if (p.kind != Prim::Bitmap || !cellRef(f, p, ref) || all.count(ref.key))
                continue;
            all.emplace(ref.key, ref);
            frameTexels += size_t(ref.lw) * ref.lh;
            if (!texels.count(ref.key))
            {
                missing.emplace(ref.key, ref);
                missingTexels += size_t(ref.lw) * ref.lh;
            }
        }
        if (missing.empty())
            return;
        if (poolUsed + missingTexels > poolCap)
        {
            // A new buffer: the old one may still be read by frames in flight (their command
            // buffers retain it). Grow and keep the cache, or start over with this frame only.
            const bool reset = poolUsed + missingTexels > kMaxTexels;
            const size_t need = reset ? frameTexels : poolUsed + missingTexels;
            const size_t cap = std::max(need, reset ? kInitialTexels : poolCap * 2);
            id<MTLBuffer> grown = [device newBufferWithLength:cap * 4
                                                      options:MTLResourceStorageModeShared];
            if (!grown)
                return;
            if (reset)
            {
                texels.clear();
                poolUsed = 0;
                missing = all;
            }
            else
                std::memcpy(grown.contents, pool.contents, poolUsed * 4);
            pool = grown;
            poolCap = cap;
        }
        // Appending to unused space never touches texels a frame in flight reads.
        uint32_t* dst = static_cast<uint32_t*>(pool.contents);
        for (const auto& [key, ref] : missing)
        {
            const std::vector<uint32_t> t = Bitmaps::decode(std::get<1>(key), ref.src, ref.lw,
                                                            ref.lh, std::get<2>(key), ref.palette);
            std::memcpy(dst + poolUsed, t.data(), t.size() * 4);
            texels[key] = uint32_t(poolUsed);
            poolUsed += t.size();
            decodes++;
        }
    }

    // One element with its quad snapped outwards to whole logical pixels (the pixels CpuRaster
    // visits) and clipped to the screen; false if empty.
    static bool setBox(Inst& in, float x0, float y0, float x1, float y1)
    {
        in.box[0] = std::max(std::floor(x0), 0.0f);
        in.box[1] = std::max(std::floor(y0), 0.0f);
        in.box[2] = std::min(std::ceil(x1), float(kScreenWidth));
        in.box[3] = std::min(std::ceil(y1), float(kScreenHeight));
        return in.box[0] < in.box[2] && in.box[1] < in.box[3];
    }

    void push(const Inst& in, const BlendMode& blend, const PixelRect& scissor)
    {
        if (batches.empty() || !(batches.back().blend == blend) ||
            !(batches.back().scissor == scissor))
            batches.push_back(Batch{ uint32_t(insts.size()), 0, blend, scissor });
        insts.push_back(in);
        batches.back().count++;
    }

    void build(const Frame& f)
    {
        insts.clear();
        batches.clear();
        const PixelRect screen{ 0, 0, kScreenWidth, kScreenHeight };
        {
            Inst in{};
            in.k[0] = KindBox;
            in.a[2] = float(kScreenWidth);
            in.a[3] = float(kScreenHeight);
            setColor(in.color, f.clearColor);
            setBox(in, 0, 0, float(kScreenWidth), float(kScreenHeight));
            push(in, BlendMode{ ONE, ZERO }, screen);
        }
        for (const Prim& p : f.prims)
        {
            const PixelRect sc = clipToScreen(p.scissor);
            if (sc.w == 0 || sc.h == 0)
                continue;
            Inst base{};
            setColor(base.color, p.rgba);
            const auto& v = p.verts;
            auto segment = [&](Vec2 a, Vec2 b) {
                Inst in = base;
                const float r = p.lineRadiusPx;
                in.k[0] = KindSegment;
                in.a[0] = a.x, in.a[1] = a.y, in.a[2] = b.x, in.a[3] = b.y;
                in.r[0] = r;
                if (setBox(in, std::min(a.x, b.x) - r - 1, std::min(a.y, b.y) - r - 1,
                           std::max(a.x, b.x) + r + 1, std::max(a.y, b.y) + r + 1))
                    push(in, p.blend, sc);
            };
            switch (p.kind)
            {
                case Prim::Points:
                    for (const Vec2& c : v)
                    {
                        Inst in = base;
                        const float r = p.pointRadiusPx;
                        in.k[0] = KindDisc;
                        in.a[0] = c.x, in.a[1] = c.y;
                        in.r[0] = r;
                        if (setBox(in, c.x - r - 1, c.y - r - 1, c.x + r + 1, c.y + r + 1))
                            push(in, p.blend, sc);
                    }
                    break;
                case Prim::Lines:
                    for (size_t i = 0; i + 1 < v.size(); i += 2)
                        segment(v[i], v[i + 1]);
                    break;
                case Prim::LineStrip:
                    for (size_t i = 0; i + 1 < v.size(); i++)
                        segment(v[i], v[i + 1]);
                    break;
                case Prim::EdgeStrip:
                    for (size_t i = 0; i + 1 < v.size(); i++)
                    {
                        Vec2 a = v[i], b = v[i + 1];
                        Inst in = base;
                        in.k[0] = KindEdge;
                        in.k[1] = uint32_t(p.edge);
                        bool okBox;
                        if (p.edge == Prim::EdgeR || p.edge == Prim::EdgeL)
                        {
                            if (a.y == b.y)
                                continue;
                            if (a.y > b.y)
                                std::swap(a, b);
                            const bool right = p.edge == Prim::EdgeR;
                            okBox = setBox(in, right ? std::min(a.x, b.x) - 1 : 0, a.y - 1,
                                           right ? float(kScreenWidth) : std::max(a.x, b.x) + 1,
                                           b.y + 1);
                        }
                        else
                        {
                            if (a.x == b.x)
                                continue;
                            if (a.x > b.x)
                                std::swap(a, b);
                            const bool below = p.edge == Prim::EdgeB;
                            okBox = setBox(in, a.x - 1, below ? std::min(a.y, b.y) - 1 : 0,
                                           b.x + 1,
                                           below ? float(kScreenHeight) : std::max(a.y, b.y) + 1);
                        }
                        in.a[0] = a.x, in.a[1] = a.y, in.a[2] = b.x, in.a[3] = b.y;
                        if (okBox)
                            push(in, p.blend, sc);
                    }
                    break;
                case Prim::Rects:
                    for (size_t i = 0; i + 1 < v.size(); i += 2)
                    {
                        Inst in = base;
                        const float r = p.lineRadiusPx;
                        in.k[0] = KindBox;
                        in.a[0] = std::min(v[i].x, v[i + 1].x);
                        in.a[1] = std::min(v[i].y, v[i + 1].y);
                        in.a[2] = std::max(v[i].x, v[i + 1].x);
                        in.a[3] = std::max(v[i].y, v[i + 1].y);
                        in.r[0] = r;
                        if (setBox(in, in.a[0] - r - 1, in.a[1] - r - 1, in.a[2] + r + 1,
                                   in.a[3] + r + 1))
                            push(in, p.blend, sc);
                    }
                    break;
                case Prim::Bitmap:
                {
                    CellRef ref;
                    if (!cellRef(f, p, ref))
                        break;
                    auto it = texels.find(ref.key);
                    if (it == texels.end())
                        break; // the pool could not grow
                    Inst in = base;
                    const Vec2 o = v[0];
                    in.k[0] = KindBitmap;
                    in.k[1] = p.filter;
                    in.k[2] = it->second;
                    in.k[3] = ref.lw;
                    in.k2[0] = ref.lh;
                    in.k2[1] = p.wrapX;
                    in.k2[2] = p.wrapY;
                    in.a[0] = o.x, in.a[1] = o.y;
                    std::copy(p.xform, p.xform + 4, in.m0);
                    in.m1[0] = p.xform[4];
                    in.m1[1] = p.xform[5];
                    in.m1[2] = float(p.w);
                    in.m1[3] = float(p.h);
                    if (setBox(in, o.x, o.y, o.x + float(p.w), o.y + float(p.h)))
                        push(in, p.blend, sc);
                    break;
                }
                case Prim::Gradient:
                {
                    Inst in = base;
                    in.k[0] = KindGradient;
                    in.a[0] = v[0].x, in.a[1] = v[0].y, in.a[2] = v[1].x, in.a[3] = v[1].y;
                    setColor(in.color, p.rgba);
                    setColor(in.color2, p.rgba2);
                    if (setBox(in, 0, 0, float(kScreenWidth), float(kScreenHeight)))
                        push(in, p.blend, sc);
                    break;
                }
            }
        }
    }

    void drawLocked(const Frame& f, id<MTLRenderCommandEncoder> enc, CGSize size, MTLPixelFormat fmt)
    {
        uploadLocked(f);
        build(f);
        const Viewport vp = MetalRenderer::fit(size);
        Uniforms u{};
        u.viewport[0] = float(size.width);
        u.viewport[1] = float(size.height);
        u.offset[0] = vp.x;
        u.offset[1] = vp.y;
        u.scale = vp.scale;
        // A fresh buffer per frame: the caller owns the command buffer, so there is no completion
        // to recycle a ring on; Metal keeps it alive until the GPU is done.
        id<MTLBuffer> ib = [device newBufferWithBytes:insts.data()
                                               length:insts.size() * sizeof(Inst)
                                              options:MTLResourceStorageModeShared];
        if (!ib)
            return;
        [enc setVertexBuffer:ib offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof u atIndex:1];
        [enc setFragmentBuffer:ib offset:0 atIndex:0];
        [enc setFragmentBytes:&u length:sizeof u atIndex:1];
        [enc setFragmentBuffer:pool offset:0 atIndex:2];
        const long dw = long(size.width), dh = long(size.height);
        id<MTLRenderPipelineState> current = nil;
        for (const Batch& b : batches)
        {
            const PixelRect& s = b.scissor;
            const long x0 = std::clamp(std::lround(float(s.x) * vp.scale + vp.x), 0L, dw);
            const long y0 = std::clamp(std::lround(float(s.y) * vp.scale + vp.y), 0L, dh);
            const long x1 = std::clamp(std::lround(float(s.x + s.w) * vp.scale + vp.x), 0L, dw);
            const long y1 = std::clamp(std::lround(float(s.y + s.h) * vp.scale + vp.y), 0L, dh);
            if (x1 <= x0 || y1 <= y0)
                continue;
            id<MTLRenderPipelineState> ps = pipeline(fmt, b.blend);
            if (!ps)
                continue;
            if (ps != current)
                [enc setRenderPipelineState:(current = ps)];
            [enc setScissorRect:MTLScissorRect{ NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0),
                                                NSUInteger(y1 - y0) }];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                  instanceCount:b.count
                   baseInstance:b.first];
        }
        // Leave the scissor as Metal's default for whatever the caller encodes next.
        [enc setScissorRect:MTLScissorRect{ 0, 0, NSUInteger(dw), NSUInteger(dh) }];
    }

    bool renderLocked(const Frame& f, int scale, std::vector<uint8_t>& out)
    {
        if (!ok || scale < 1)
            return false;
        const NSUInteger w = NSUInteger(kScreenWidth * scale), h = NSUInteger(kScreenHeight * scale);
        MTLTextureDescriptor* td =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:w
                                                            height:h
                                                         mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared; // Apple silicon / iOS: readable after completion
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex)
            return false;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        drawLocked(f, enc, CGSizeMake(w, h), MTLPixelFormatBGRA8Unorm);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
        {
            NSLog(@"MetalRenderer: offscreen render failed: %@", cb.error);
            return false;
        }
        out.resize(size_t(w) * h * 4);
        [tex getBytes:out.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        return true;
    }
};

MetalRenderer::Viewport MetalRenderer::fit(CGSize px)
{
    const double w = px.width, h = px.height;
    if (w <= 0 || h <= 0)
        return {};
    const double fitScale = std::min(w / kScreenWidth, h / kScreenHeight);
    const double s = fitScale >= 1 ? std::floor(fitScale) : fitScale;
    Viewport v;
    v.scale = float(s);
    // whole pixels; rounded, since kScreenWidth * 0.8 may come out a hair over 640
    v.x = float(std::max(std::floor((w - kScreenWidth * s) / 2 + 1e-6), 0.0));
    v.y = float(std::max(std::floor((h - kScreenHeight * s) / 2 + 1e-6), 0.0));
    return v;
}

MetalRenderer::MetalRenderer(id<MTLDevice> device, MTLPixelFormat colorFormat)
  : impl_(std::make_unique<Impl>())
{
    impl_->init(device, colorFormat);
}

MetalRenderer::MetalRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetalRenderer::~MetalRenderer() = default;

std::unique_ptr<MetalRenderer> MetalRenderer::create()
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev)
        return nullptr;
    auto r = std::unique_ptr<MetalRenderer>(new MetalRenderer(dev));
    return r->ok() ? std::move(r) : nullptr;
}

bool MetalRenderer::ok() const { return impl_->ok; }

void MetalRenderer::draw(const Frame& frame, id<MTLRenderCommandEncoder> encoder, CGSize drawablePx)
{
    if (!impl_->ok || !encoder)
        return;
    std::lock_guard<std::mutex> g(impl_->mutex);
    impl_->drawLocked(frame, encoder, drawablePx, impl_->colorFormat);
}

bool MetalRenderer::renderToBGRA(const Frame& frame, int scale, std::vector<uint8_t>& out)
{
    std::lock_guard<std::mutex> g(impl_->mutex);
    return impl_->renderLocked(frame, scale, out);
}

bool MetalRenderer::renderToPNG(const Frame& frame, const char* path, int scale)
{
    std::vector<uint8_t> px;
    if (!path || !renderToBGRA(frame, scale, px))
        return false;
    const size_t w = size_t(kScreenWidth * scale), h = size_t(kScreenHeight * scale);
    bool ok = false;
    @autoreleasepool
    {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CFDataRef data = CFDataCreate(nullptr, px.data(), CFIndex(px.size()));
        CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
        // B G R A in memory: 32-bit little endian with the alpha byte ignored (opaque screen).
        CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                       CGBitmapInfo(kCGBitmapByteOrder32Little) | CGBitmapInfo(kCGImageAlphaNoneSkipFirst), dp,
                                       nullptr, false, kCGRenderingIntentDefault);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                                    CFSTR("public.png"), 1, nullptr);
        if (dst && img)
        {
            CGImageDestinationAddImage(dst, img, nullptr);
            ok = CGImageDestinationFinalize(dst);
        }
        if (dst)
            CFRelease(dst);
        CGImageRelease(img);
        CGDataProviderRelease(dp);
        CFRelease(data);
        CGColorSpaceRelease(cs);
    }
    return ok;
}

void MetalRenderer::uploadBitmaps(const Frame& frame)
{
    if (!impl_->ok)
        return;
    std::lock_guard<std::mutex> g(impl_->mutex);
    impl_->uploadLocked(frame);
}

void MetalRenderer::uploadBitmaps(const Ft812Model& model)
{
    if (auto f = model.latestFrame())
        uploadBitmaps(*f);
}

uint64_t MetalRenderer::bitmapDecodes() const
{
    std::lock_guard<std::mutex> g(impl_->mutex);
    return impl_->decodes;
}

void MetalRenderer::attachSnapshotHook(Ft812Model& model)
{
    Ft812Model* m = &model;
    model.snapshotHook = [this, m](PixelRect r, uint32_t addr, uint32_t fmt) -> bool {
        constexpr uint32_t kArgb8 = 0x20; // CMD_SNAPSHOT2 format the core uses (snapshots.cpp)
        if (fmt != kArgb8 || r.w <= 0 || r.h <= 0)
            return false;
        uint8_t* d = m->memAt(addr, uint32_t(r.w) * uint32_t(r.h) * 4);
        if (!d)
            return false;
        // EVE snapshots the list on screen: the last swapped frame.
        std::vector<uint8_t> px;
        const auto f = m->latestFrame();
        const bool have = f && renderToBGRA(*f, 1, px);
        for (int y = 0; y < r.h; y++)
            for (int x = 0; x < r.w; x++)
            {
                const int sx = r.x + x, sy = r.y + y;
                uint8_t* o = d + (size_t(y) * r.w + x) * 4;
                const bool in = have && sx >= 0 && sy >= 0 && sx < kScreenWidth && sy < kScreenHeight;
                const uint8_t* s = in ? &px[(size_t(sy) * kScreenWidth + sx) * 4] : nullptr;
                o[0] = s ? s[0] : 0; // B
                o[1] = s ? s[1] : 0; // G
                o[2] = s ? s[2] : 0; // R
                o[3] = 255;          // A: the screen is opaque
            }
        return true; // the model notes the write
    };
}

void MetalRenderer::detachSnapshotHook(Ft812Model& model) { model.snapshotHook = nullptr; }
