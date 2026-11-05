#include "Ft812Model.h"

#include "Bitmaps.h"

#include <algorithm>
#include <cstring>

Ft812Model::Ft812Model()
  : ramG_(kRamGSize, 0),
    ramDl_(kRamDlSize, 0),
    copro_(*this),
    pageGen_(new std::atomic<uint32_t>[kPages])
{
    for (uint32_t i = 0; i < kPages; i++)
        pageGen_[i].store(0, std::memory_order_relaxed);
}

Ft812Model::~Ft812Model() = default;

void Ft812Model::getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height)
{
    // CMD_GETPROPS: end address, width and height of the last CMD_LOADIMAGE.
    addr = imgEnd_;
    width = imgWidth_;
    height = imgHeight_;
}

// ---- command stream ----

void Ft812Model::WriteData32(uint32_t data)
{
    const uint8_t b[4] = { uint8_t(data), uint8_t(data >> 8), uint8_t(data >> 16),
                           uint8_t(data >> 24) };
    pending_.insert(pending_.end(), b, b + 4);
}

uint8_t Ft812Model::sendString(const char* string, uint32_t length)
{
    // API_SendString (platforms/imx/3rdparty/FT812/API_Layer.cpp): length 0 or past the end means
    // the whole string; NUL; zero padding to 4. Returns the byte count as a uint8_t, like IMX.
    const uint32_t real = uint32_t(std::strlen(string));
    if (length == 0 || length > real)
        length = real;
    pending_.insert(pending_.end(), string, string + length);
    uint32_t size = length + 1;
    pending_.push_back(0);
    while (size & 3)
    {
        pending_.push_back(0);
        size++;
    }
    return uint8_t(size);
}

void Ft812Model::endCoProList() { flushPending(); }

void Ft812Model::writeDataToCMD(const uint8_t* data, uint32_t size)
{
    // The IMX driver writes straight into the FIFO and bumps REG_CMD_WRITE itself, so the data
    // executes now, padded to a multiple of 4.
    if (data && size)
        pending_.insert(pending_.end(), data, data + size);
    while (pending_.size() & 3)
        pending_.push_back(0);
    flushPending();
}

void Ft812Model::flushPending()
{
    if (pending_.empty())
        return;
    copro_.feed(pending_.data(), pending_.size());
    pending_.clear();
}

// ---- memory ----

uint8_t* Ft812Model::memAt(uint32_t addr, uint32_t len)
{
    if (uint64_t(addr) + len <= kRamGSize)
        return ramG_.data() + addr;
    if (addr >= kRamDlAddr && uint64_t(addr) + len <= uint64_t(kRamDlAddr) + kRamDlSize)
        return ramDl_.data() + (addr - kRamDlAddr);
    return nullptr;
}

void Ft812Model::noteWrite(uint32_t addr, uint32_t len)
{
    if (len == 0 || addr >= kRamGSize)
        return;
    const uint32_t end = std::min<uint64_t>(uint64_t(addr) + len, kRamGSize) - 1;
    const uint32_t stamp = writeStamp_.fetch_add(1, std::memory_order_relaxed) + 1;
    for (uint32_t p = addr >> kPageShift; p <= (end >> kPageShift); p++)
        pageGen_[p].store(stamp, std::memory_order_release);
}

uint32_t Ft812Model::ramGGeneration(uint32_t addr, uint32_t len) const
{
    if (len == 0 || addr >= kRamGSize)
        return 0;
    const uint32_t end = std::min<uint64_t>(uint64_t(addr) + len, kRamGSize) - 1;
    uint32_t gen = 0;
    for (uint32_t p = addr >> kPageShift; p <= (end >> kPageShift); p++)
        gen = std::max(gen, pageGen_[p].load(std::memory_order_acquire));
    return gen;
}

void Ft812Model::writeDataRAMG(const uint8_t* data, uint32_t size, uint32_t dest)
{
    // API_LIB_WriteDataRAMG: bytes as given (the trailing zero pad lands past `size`).
    uint8_t* p = memAt(dest, size);
    if (!p || !data)
        return;
    std::memcpy(p, data, size);
    noteWrite(dest, size);
}

void Ft812Model::writeFontRAMG(const uint8_t* data, uint32_t size, uint32_t dest)
{
    // API_LIB_WriteFontRAMG: the 148-byte metric block's glyph pointer (bytes 144..147) is
    // replaced with dest + 148, little endian.
    uint8_t* p = memAt(dest, size);
    if (!p || !data || size < 148)
        return;
    std::memcpy(p, data, size);
    const uint32_t glyphs = dest + 148;
    const uint8_t b[4] = { uint8_t(glyphs), uint8_t(glyphs >> 8), uint8_t(glyphs >> 16),
                           uint8_t(glyphs >> 24) };
    std::memcpy(p + 144, b, 4);
    noteWrite(dest, size);
}

void Ft812Model::readDataRAMG(uint8_t* data, uint32_t size, uint32_t src)
{
    if (!data)
        return;
    const uint8_t* p = memAt(src, size);
    if (p)
        std::memcpy(data, p, size);
    else
        std::memset(data, 0, size);
}

void Ft812Model::readBackDataRAMG(uint8_t* data, uint32_t size, uint32_t src)
{
    // API_LIB_ReadBackDataRAMG: reads ascending from src - 1 and stores from the end backwards.
    if (!data || size == 0)
        return;
    const uint8_t* p = src > 0 ? memAt(src - 1, size) : nullptr;
    for (uint32_t i = 0; i < size; i++)
        data[size - 1 - i] = p ? p[i] : 0;
}

// ---- stream DMA (NES video): synchronous copy ----

void Ft812Model::initStreamDma(uint16_t byteResolution,
                               uint16_t streamWidth,
                               uint16_t streamHeight,
                               uint8_t* streamBuf,
                               uint32_t destMemAddress)
{
    if (streamInit_)
        return;
    streamBuf_ = streamBuf;
    streamDest_ = destMemAddress;
    streamSize_ = uint32_t(byteResolution) * streamWidth * streamHeight;
    streamInit_ = true;
}

bool Ft812Model::updateStreamDma(bool lastFrame)
{
    if (!streamInit_)
        return true;
    if (!lastFrame)
        return false; // like IMX step 0: only a finished frame starts a transfer
    writeDataRAMG(streamBuf_, streamSize_, streamDest_);
    return true;
}

void Ft812Model::stopStreamDma() { streamInit_ = false; }

// ---- frames ----

void Ft812Model::swap()
{
    auto frame = std::make_shared<Frame>();
    frame->serial = ++frameSerial_;
    dl_.decode(ramDl_.data(), ramDl_.size(), *frame);
    snapshotRamG(*frame);
    std::atomic_store_explicit(
      &latest_, std::shared_ptr<const Frame>(std::move(frame)), std::memory_order_release);
}

std::shared_ptr<const Frame> Ft812Model::latestFrame() const
{
    return std::atomic_load_explicit(&latest_, std::memory_order_acquire);
}

void Ft812Model::snapshotRamG(Frame& frame)
{
    // Runs on the thread that writes RAM_G (CMD_SWAP), so bytes and generations are consistent.
    // Whole pages are copied so the blocks stay the same from frame to frame (a font's block does
    // not depend on which glyphs this frame draws) and are shared until a page changes.
    std::vector<bool> need(kPages, false);
    auto mark = [&](uint32_t addr, uint32_t len) {
        if (len == 0 || uint64_t(addr) + len > kRamGSize)
            return;
        for (uint32_t p = addr >> kPageShift; p <= (addr + len - 1) >> kPageShift; p++)
            need[p] = true;
    };
    for (const Prim& p : frame.prims)
    {
        if (p.kind != Prim::Bitmap)
            continue;
        uint32_t addr = 0, bytes = 0;
        if (Bitmaps::cellRange(p, addr, bytes))
            mark(addr, bytes);
        mark(p.paletteAddr, Bitmaps::paletteBytes(p.fmt));
    }

    std::vector<RamGBlock> blocks;
    for (uint32_t p = 0; p < kPages;)
    {
        if (!need[p])
        {
            p++;
            continue;
        }
        uint32_t end = p;
        while (end < kPages && need[end])
            end++;
        RamGBlock b;
        b.addr = p << kPageShift;
        for (uint32_t q = p; q < end; q++)
            b.pageGens.push_back(pageGen_[q].load(std::memory_order_relaxed));
        const size_t size = size_t(end - p) << kPageShift;
        for (const RamGBlock& old : ramGBlocks_)
            if (old.addr == b.addr && old.bytes->size() == size && old.pageGens == b.pageGens)
                b.bytes = old.bytes;
        if (!b.bytes)
            b.bytes = std::make_shared<const std::vector<uint8_t>>(ramG_.begin() + b.addr,
                                                                   ramG_.begin() + b.addr + size);
        blocks.push_back(std::move(b));
        p = end;
    }
    ramGBlocks_ = blocks;
    frame.ramG = std::move(blocks);
}
