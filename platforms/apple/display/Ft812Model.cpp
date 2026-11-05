#include "Ft812Model.h"

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
    // CMD_GETPROPS results arrive with CMD_LOADIMAGE support (Task 5b).
    addr = 0;
    width = 0;
    height = 0;
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
    std::atomic_store_explicit(
      &latest_, std::shared_ptr<const Frame>(std::move(frame)), std::memory_order_release);
}

std::shared_ptr<const Frame> Ft812Model::latestFrame() const
{
    return std::atomic_load_explicit(&latest_, std::memory_order_acquire);
}
