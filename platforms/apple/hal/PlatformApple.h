#pragma once
#include "ConfigManager.h"
#include "DisplayDriver.h"
#include "FileSystemPosix.h"
#include "Ft812Model.h"
#include "InputApple.h"
#include "MidiCoreMidi.h"
#include "Platform.h"
#include "StubsApple.h"
#include "TimerApple.h"

#include <string>

class PlatformApple final : public Platform
{
  public:
    // sdRootPath: host folder standing in for the SD card (created, with the folder layout the
    // core expects on a fresh card, if missing).
    explicit PlatformApple(const std::string& sdRootPath);

    void setPowerMode(bool mode) override { (void)mode; }
    void resetMCU() override {}
    void runBootloader(char*) override {}
    void setPowerButtonCallback(PowerButtonCallback cb) override { powerButtonCallback_ = cb; }
    bool getPowerButtonState() override { return false; }
    bool readForceStartUp() override { return false; }

    Sgtl5000* getCodecAudio() override { return &codec_; }
    Radio* getRadio() override { return &radio_; }
    SwitchGroup* getButtons() override { return &input_.buttons(); }
    SwitchGroup* getPads() override { return &input_.pads(); }
    Leds* getLeds() override { return &leds_; }
    Battery* getBattery() override { return &battery_; }
    Timer* getTimer() override { return &timer_; }
    Encoder* getEncoder() override { return &input_.encoder(); }
    DisplayDriver* getDisplayDriver() override { return &display_; }
    UsbAudio* getUsbAudio() override { return &usbAudio_; }
    UartMidi* getUartMidi() override { return &uartMidi_; }
    UsbMidi* getUsbMidi() override { return &usbMidi_; }
    ConfigManager* getConfigManager() override { return &config_; }
    FileSystem* getFileSystem() override { return &fileSystem_; }
    CrcCalculator* getCrcCalculator() override { return &crc_; }
    SystemInfo* getSystemInfo() override { return &systemInfo_; }
    Usb* getUsb() override { return &usb_; }

    InputApple& input() { return input_; }
    FileSystemPosix& fileSystem() { return fileSystem_; }
    Ft812Model& display() { return display_; }
    MidiCoreMidi& midi() { return usbMidi_; } // the shared sender (destination, offset, output)
    PowerButtonCallback powerButtonCallback() const { return powerButtonCallback_; }

  private:
    // Wires InputApple to the core's button/pad/encoder handlers (what initHardware does on IMX).
    void connectInputToCore();

    FileSystemPosix fileSystem_;
    ConfigManagerApple config_;
    TimerApple timer_;
    InputApple input_;
    Ft812Model display_;
    RadioApple radio_;
    BatteryApple battery_;
    Sgtl5000Apple codec_;
    LedsApple leds_;
    UsbApple usb_;
    UsbAudioApple usbAudio_;
    MidiCoreMidi usbMidi_;
    MidiCoreMidi uartMidi_{ &usbMidi_ }; // shares usbMidi_'s CoreMIDI sender
    CrcCalculatorApple crc_;
    SystemInfo systemInfo_{ "apple" };
    PowerButtonCallback powerButtonCallback_ = nullptr;
};
