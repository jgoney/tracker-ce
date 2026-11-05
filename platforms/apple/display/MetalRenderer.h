#pragma once
// Metal renderer for decoded FT812 frames (Frame.h). Every primitive element is one instanced quad
// whose fragment shader evaluates the same signed distances and coverage as CpuRaster (the
// reference): points are discs of radius pointRadiusPx, lines capsules of radius lineRadiusPx,
// rects boxes grown by lineRadiusPx with round corners, edge strips, bitmap cells (any FT81x
// format, decoded on the CPU by Bitmaps::decode, sampled NEAREST/BILINEAR with BORDER/REPEAT and
// BITMAP_TRANSFORM) and gradients. Coverage is clamp((r - d) * scale + 0.5, 0, 1): one device
// pixel of feathering. Each EVE blend mode is a fixed-function pipeline, the scissor is
// setScissorRect, and elements blend in display-list order.
//
// Threads: a renderer may be used from several threads (an internal mutex serialises it): draw()
// on the main/display thread and the CMD_SNAPSHOT2 hook on the tracker thread. Frames carry their
// own copy of the RAM_G bytes they read (Frame::ramG), so rendering never reads live RAM_G.
//
// Includable from C++: Metal types appear only under __OBJC__. C++ callers use create().
#include "Frame.h"

#include <CoreGraphics/CGGeometry.h>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

class Ft812Model;

class MetalRenderer
{
  public:
    // Maps the 800x480 logical screen into a drawable: device px = logical px * scale + (x, y).
    // Integer scale (>= 1) when the screen fits, else uniform downscale; centred (letterbox), with
    // whole-pixel offsets.
    struct Viewport
    {
        float scale = 1, x = 0, y = 0;
    };
    static Viewport fit(CGSize drawablePx);

#ifdef __OBJC__
    // `colorFormat` is the pixel format of the render targets draw() encodes into.
    explicit MetalRenderer(id<MTLDevice> device,
                           MTLPixelFormat colorFormat = MTLPixelFormatBGRA8Unorm);

    // Encodes `frame` into an open render pass whose colour attachment is `drawablePx` large:
    // the screen area is filled with the frame's clear colour, the letterbox is left untouched.
    void draw(const Frame& frame, id<MTLRenderCommandEncoder> encoder, CGSize drawablePx);
#endif

    // The system default device; nullptr without Metal or if the shaders fail to compile.
    static std::unique_ptr<MetalRenderer> create();
    ~MetalRenderer();
    MetalRenderer(const MetalRenderer&) = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // False if the pipelines could not be built (the reason was logged).
    bool ok() const;

    // Offscreen: (800 * scale) x (480 * scale) pixels, 4 bytes each (B, G, R, A), row-major from
    // the top. Waits for the GPU.
    bool renderToBGRA(const Frame& frame, int scale, std::vector<uint8_t>& out);
    // Offscreen to an opaque RGB PNG file.
    bool renderToPNG(const Frame& frame, const char* path, int scale = 1);

    // Decodes the bitmap cells `frame` draws into the GPU texel cache, keyed by (RAM_G address,
    // format, stride, rows, palette, generation of those bytes). draw() and the offscreen paths
    // call it themselves; calling it early only moves the decoding cost.
    void uploadBitmaps(const Frame& frame);
    // Same, for the model's latest frame.
    void uploadBitmaps(const Ft812Model& model);
    // Number of cells decoded so far (cache misses), for tests.
    uint64_t bitmapDecodes() const;

    // Installs model.snapshotHook: CMD_SNAPSHOT2 renders the model's latest frame offscreen and
    // writes the requested rectangle into RAM_G as ARGB8 (bytes B, G, R, A with A = 255; pixels
    // outside the screen are black). Other formats return false (the model fills black).
    // Attach before the tracker thread starts and detach after it stops; the renderer must
    // outlive the hook.
    void attachSnapshotHook(Ft812Model& model);
    static void detachSnapshotHook(Ft812Model& model);

    struct Impl;

  private:
    explicit MetalRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
