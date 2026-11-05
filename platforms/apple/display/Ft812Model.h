#pragma once
// FT812 (EVE2) model at the DisplayDriver level: RAM_G, RAM_DL and REG_CMD_DL, a coprocessor that
// executes the command stream synchronously (CoproDecoder) and a display-list decoder that turns
// ram_dl into a Frame on every CMD_SWAP. The coprocessor is never busy.
#include "CoproDecoder.h"
#include "DisplayDriver.h"
#include "DlDecoder.h"
#include "Frame.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Ft812Model : public DisplayDriver
{
  public:
    static constexpr uint32_t kRamGSize = 1024 * 1024; // RAM_G_SIZE
    static constexpr uint32_t kRamDlAddr = 0x300000;   // RAM_DL
    static constexpr uint32_t kRamDlSize = 8 * 1024;   // FT_DL_SIZE

    Ft812Model();
    ~Ft812Model();

    void setPowerState(bool state) override { powered_ = state; }
    void setBrightness(int value) override { brightness_ = value; }
    void setRotation(int value) override { rotation_ = value; }

    void getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height) override;
    uint32_t getDisplayListOffset() override { return dlOffset_; }
    void IncCMDOffset(uint16_t) override {}
    void WriteData32(uint32_t data) override;

    void beginCoProList() override {}
    void beginCoProListNoCheck() override {}
    void endCoProList() override;
    void awaitCoProEmpty() override {}
    uint8_t isCoProEmpty() override { return 1; }
    void writeDataRAMG(const uint8_t* data, uint32_t size, uint32_t dest) override;
    void writeFontRAMG(const uint8_t* data, uint32_t size, uint32_t dest) override;
    void readDataRAMG(uint8_t* data, uint32_t size, uint32_t src) override;
    void readBackDataRAMG(uint8_t* data, uint32_t size, uint32_t src) override;
    uint8_t sendString(const char* string, uint32_t length) override;
    void writeDataToCMD(const uint8_t* data, uint32_t size) override;

    void initStreamDma(uint16_t byteResolution,
                       uint16_t streamWidth,
                       uint16_t streamHeight,
                       uint8_t* streamBuf,
                       uint32_t destMemAddress) override;
    bool updateStreamDma(bool lastFrame) override;
    void stopStreamDma() override;

    // --- host side ---
    // CMD_SNAPSHOT2: renders `rect` (screen pixels) into RAM_G at `addr` in format `fmt` and
    // returns true. Unset or returning false: the model fills the area black.
    std::function<bool(PixelRect rect, uint32_t addr, uint32_t fmt)> snapshotHook;

    bool powered() const { return powered_; }
    int brightness() const { return brightness_; }
    int rotation() const { return rotation_; }

    // Last frame published by CMD_SWAP (nullptr before the first). Safe from any thread: the frame
    // is immutable and carries its own copy of the RAM_G bytes its bitmaps read (Frame::ramG).
    std::shared_ptr<const Frame> latestFrame() const;

    // Changes whenever any byte in [addr, addr+len) of RAM_G may have changed (texture caching).
    uint32_t ramGGeneration(uint32_t addr, uint32_t len) const;

    const uint8_t* ramG() const { return ramG_.data(); }
    const uint8_t* ramDl() const { return ramDl_.data(); }

    // --- used by CoproDecoder ---
    // Pointer to `len` bytes at EVE address `addr` (RAM_G or RAM_DL), or nullptr if the range
    // is outside both. Writing through it must be followed by noteWrite().
    uint8_t* memAt(uint32_t addr, uint32_t len);
    void noteWrite(uint32_t addr, uint32_t len);
    uint32_t& dlOffset() { return dlOffset_; }
    uint8_t* ramDlMutable() { return ramDl_.data(); }
    // CMD_LOADIMAGE result, returned by CMD_GETPROPS / getImgProps.
    void setImgProps(uint32_t endAddr, uint32_t width, uint32_t height)
    {
        imgEnd_ = endAddr;
        imgWidth_ = width;
        imgHeight_ = height;
    }
    // Decodes ram_dl and publishes the frame (CMD_SWAP).
    void swap();

  private:
    void flushPending();
    // Copies the RAM_G pages the frame's bitmap prims read into frame.ramG (see Frame.h).
    void snapshotRamG(Frame& frame);

    static constexpr uint32_t kPageShift = 12; // 4 KB generation pages
    static constexpr uint32_t kPages = kRamGSize >> kPageShift;

    std::vector<uint8_t> ramG_;
    std::vector<uint8_t> ramDl_;
    uint32_t dlOffset_ = 0;
    std::vector<uint8_t> pending_; // command bytes written since the last endCoProList
    CoproDecoder copro_;
    DlDecoder dl_;
    uint64_t frameSerial_ = 0;
    std::shared_ptr<const Frame> latest_;
    std::vector<RamGBlock> ramGBlocks_; // the last frame's blocks, reused while unchanged
    std::unique_ptr<std::atomic<uint32_t>[]> pageGen_;
    std::atomic<uint32_t> writeStamp_{ 0 };

    // stream DMA (NES video): copied synchronously
    uint8_t* streamBuf_ = nullptr;
    uint32_t streamDest_ = 0;
    uint32_t streamSize_ = 0;
    bool streamInit_ = false;

    uint32_t imgEnd_ = 0, imgWidth_ = 0, imgHeight_ = 0;

    bool powered_ = false;
    int brightness_ = 0;
    int rotation_ = 0;
};
