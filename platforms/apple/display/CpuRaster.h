#pragma once
// Reference rasteriser for a decoded Frame: slow, straightforward EVE primitive semantics
// (see INVENTORY.md). Ground truth for the Metal renderer. Bitmap texels are read from the
// model's RAM_G at render time.
#include "Frame.h"

#include <cstdint>
#include <vector>

class Ft812Model;

namespace CpuRaster
{
// 800x480 RGB, 3 bytes per pixel, row-major from the top.
std::vector<uint8_t> render(const Frame& frame, const Ft812Model& model);

// Writes a binary PPM (P6). Returns false if the file cannot be written.
bool toPPM(const Frame& frame, const Ft812Model& model, const char* path);
} // namespace CpuRaster
