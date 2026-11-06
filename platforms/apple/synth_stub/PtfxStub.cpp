// Stand-in for core/effector/ptfx_loader.cpp (excluded from the Apple build).
//
// .ptfx plugins are Thumb machine code for the Cortex-M target, so they cannot run on this
// platform: headers are never readable and loadPlugin() always fails ("not supported on this
// platform"). PtfxManager then simply lists no plugins.
#include "ptfx_loader.h"

#include <cstring>

namespace
{
alignas(16) uint8_t s_codeBuffer[kPtfxCodeBufferSize];
alignas(16) uint8_t s_scratchBuffer[kPtfxScratchTopOffset - kPtfxScratchOffset];
const PtfxHostApi s_hostApi = {};
} // namespace

uint8_t* PtfxLoader::getCodeBuffer()
{
    return s_codeBuffer;
}
uint32_t PtfxLoader::getMaxCodeSize()
{
    return kPtfxCodeBufferSize;
}
void* PtfxLoader::getScratchBuffer()
{
    return s_scratchBuffer;
}
uint32_t PtfxLoader::getMaxScratchSize()
{
    return sizeof(s_scratchBuffer);
}
const PtfxHostApi* PtfxLoader::getHostApi()
{
    return &s_hostApi;
}
void PtfxLoader::syncInstructionCache(void*, uint32_t) {}

bool PtfxLoader::readHeader(const char*, PtfxHeader& outHeader)
{
    std::memset(&outHeader, 0, sizeof(outHeader));
    return false;
}

bool PtfxLoader::loadPlugin(const char*,
                            PtfxHeader& outHeader,
                            PtfxInitFunc& outInit,
                            PtfxSetParamFunc& outSetParam,
                            PtfxProcessBlockFunc& outProcessBlock)
{
    std::memset(&outHeader, 0, sizeof(outHeader));
    outInit = nullptr;
    outSetParam = nullptr;
    outProcessBlock = nullptr;
    return false;
}
