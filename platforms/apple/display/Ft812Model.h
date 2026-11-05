#pragma once
#include "DisplayDriver.h"

// Minimal FT812 stand-in: accepts every write and reports the coprocessor as always empty.
// Task 5a replaces the bodies with a real command-stream / display-list model.
class Ft812Model : public DisplayDriver
{
  public:
    void setPowerState(bool state) override { powered_ = state; }
    void setBrightness(int value) override { brightness_ = value; }
    void setRotation(int value) override { rotation_ = value; }

    void getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height) override;
    uint32_t getDisplayListOffset() override { return 0; }
    void IncCMDOffset(uint16_t) override {}
    void WriteData32(uint32_t) override {}

    void beginCoProList() override {}
    void beginCoProListNoCheck() override {}
    void endCoProList() override {}
    void awaitCoProEmpty() override {}
    uint8_t isCoProEmpty() override { return 1; }
    void writeDataRAMG(const uint8_t*, uint32_t, uint32_t) override {}
    void writeFontRAMG(const uint8_t*, uint32_t, uint32_t) override {}
    void readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    void readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    uint8_t sendString(const char*, uint32_t) override { return 1; }
    void writeDataToCMD(const uint8_t*, uint32_t) override {}

    void initStreamDma(uint16_t, uint16_t, uint16_t, uint8_t*, uint32_t) override {}
    bool updateStreamDma(bool) override { return true; }
    void stopStreamDma() override {}

    bool powered() const { return powered_; }

  private:
    bool powered_ = false;
    int brightness_ = 0;
    int rotation_ = 0;
};
