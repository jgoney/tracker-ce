#include "Platform.h"
#include "Profiler.h"
#include "TrackerImpl.h"
#include "mtWavetableEnvelopes.h"
#include "core/interface.h"
#include "elapsedMillis.h"
#include "fileManager.h"
#include "mtAudioEngine.h"
#include "mtAudioExport.h"
#include "mtConfig.h"
#include "mtHardware.h"
#include "mtMidi.h"
#include "mtPadsBacklight.h"
#include "mtSequencer.h"
#include "mtSleep.h"
#include "mtSliceDetector.h"

#include <cr_section_macros.h>

//-----------------------------------  EXTERNAL RAM CONFIGURATION --------------------------------
#if defined(PLATFORM_APPLE)
// Mach-O rejects ELF section names; the host just uses ordinary static storage.
#define EXT_RAM_ATTR(sec) __attribute__((used))
#define EXT_RAM_ATTR_ALIGNED(sec, align) __attribute__((aligned(align), used))
#else
#define EXT_RAM_ATTR(sec) __attribute__((section(sec), used))
#define EXT_RAM_ATTR_ALIGNED(sec, align) __attribute__((section(sec), aligned(align), used))
#endif
// clang-format off
EXT_RAM_ATTR(".sample_memory") int16_t externalRamSampleMemory[SAMPLE_MEMORY_SIZE / 2];
EXT_RAM_ATTR(".delay_memory") int16_t externalRamDelayBuff[DELAY_MEMORY_SIZE / 2];
EXT_RAM_ATTR(".reverb_memory") uint16_t externalRamReverbBuff[REVERB_MEMORY_SIZE / 2];
EXT_RAM_ATTR_ALIGNED(".scratch_memory", 32) uint8_t externalRamScratchBuff[SHARED_SCRATCH_MEMORY_SIZE];

// clang-format on
int16_t* externalRamEffectsBuff = externalRamSampleMemory + (EDIT_BUFF_OFFSET / 2);
int16_t* externalRamSampleMemoryBegin = externalRamSampleMemory;

//--------------------------------------------------------------------------------------------------------------
static TrackerImpl instance;

extern void onPowerButtonChange(int value);
extern void onSDCardSlotChange(bool state);

void startDisplay(Platform* platform)
{
    [[maybe_unused]] bool powerButtonState = platform->getPowerButtonState();

    display.begin(platform->getDisplayDriver(), platform->getTimer());
#ifdef PLATFORM_IMX
    // delay to hide white screen before driver initialize the screen
    platform->getTimer()->delay(200);
#endif

#ifdef TRACKER_MINI
    // only in release build mode (always start device immediately in debug mode)
#if !defined(DEBUG) && !defined(FORCED_AUTOSTART)
    if (platform->readForceStartUp() == false &&
        (!powerButtonState || platform->getBattery()->isEmpty()))
    {
        Battery* battery = platform->getBattery();
        display.showBatteryScreen(
          battery->isEmpty(), battery->getCharging(), battery->getLevel(), battery->getVoltage());
        platform->getTimer()->delay(100);
        display.turnOn();
        display.showBatteryScreen(
          battery->isEmpty(), battery->getCharging(), battery->getLevel(), battery->getVoltage());
        platform->getTimer()->delay(2000);
        platform->setPowerMode(false);
    }
#endif
#endif

    display.turnOn();
}

Tracker* Tracker::getInstance()
{
    return &instance;
}

TrackerImpl* TrackerImpl::getInstance()
{
    return &instance;
}

Platform* TrackerImpl::getPlatform()
{
    return platform;
}

#ifdef FEATURE_SYNTH
Synth& TrackerImpl::getSynth()
{
    return synth;
}
#endif

const char* Tracker::getVersion()
{
    return "1.0";
}

void TrackerImpl::init(Platform* platform)
{
    this->platform = platform;

    initWavetableEnvelopes();

    Timer* timer = getPlatform()->getTimer();
    elapsedMicros::init(timer);
    elapsedMillis::init(timer);
    elapsedMicrosAudio::init(timer);
    elapsedMillisAudio::init(timer);
#ifdef PLATFORM_IMX
    AudioStream::setUsbAudio(platform->getUsbAudio());
#endif

    midiInit(platform->getUartMidi(), platform->getUsbMidi());

    platform->setPowerButtonCallback(onPowerButtonChange);
    platform->getFileSystem()->setMountEventCallback(onSDCardSlotChange);

#ifdef FEATURE_SYNTH
    synth.init(platform, [this]() { return sequencer.getCurrentTempo(); });

    engine.init(platform, &synth);
#else
    engine.init(platform, nullptr);
#endif
    sequencer.init();
    sequencer.setCrcCalculator(platform->getCrcCalculator());
    padsBacklight.init(500, platform->getLeds());
    exporter.init(platform->getFileSystem());
    Profiler::getInstance()->init();
    newFileManager.setCrcCalculator(platform->getCrcCalculator());

    initConfig(platform);
    // UI init at the end
#ifdef FEATURE_SYNTH
    mtInterface.begin(platform, &synth);
#else
    mtInterface.begin(platform, nullptr);
#endif

    bool stereoSampleMode = false;
#ifdef PLATFORM_IMX
    stereoSampleMode = true;
#endif

#ifdef FEATURE_SYNTH
    newFileManager.init(platform->getFileSystem(), &synth, stereoSampleMode);
#else
    newFileManager.init(platform->getFileSystem(), nullptr, stereoSampleMode);
#endif

    interfaceGlobals.devMode = platform->getFileSystem()->exists("/.develop");

#if defined(PLATFORM_IMX) && defined(ENABLE_DEV_CONSOLE)
    if (interfaceGlobals.devMode)
    {
        // The serial port carries the log and the development console.
        Usb* usb = platform->getUsb();
        usb->setClassState(Usb::ClassType::kSerial, true);
        usb->reset();
    }
#endif
}

void TrackerImpl::update()
{
    if (lowPower.getLowPowerState() == shutdownStateSleep)
    {
        return;
    }

    engine.updateSdStream();
    display.update();
    engine.updateSdStream();
    padsBacklight.update();
    exporter.update();
    sliceDetector.update();
    newFileManager.update();
    mtInterface.update();
    engine.updateSdStream();
    sequencer.update();
}

void TrackerImpl::updateAudioBuffers(void* left, void* right, int length)
{
    if (AudioStream::renderMode)
    {
        return;
    }
    if (platform != nullptr)
    {
        midiSetAudioBlockStartUs(platform->getTimer()->micros());
    }
    Profiler::getInstance()->startMeasuringAudio();
    midiUpdate();

    // Exact hardware I2S sample rate:
    // - IMX (Tracker+ and Tracker Mini): PLL4 = 24 MHz * (37 + 15168/24000) / 2 / 4 / 10 / 256 = 44100.0 Hz
    // - MK66 (Original Tracker): F_CPU = 180 MHz * 16 / 255 / 256 = 44117.6470588 Hz
#ifdef PLATFORM_IMX
    static constexpr float kAudioSampleRate = 44100.0f;
#else
    static constexpr float kAudioSampleRate = 44117.6470588f;
#endif

    float updateIntervalSeconds = length / kAudioSampleRate;
    sequencer.tick(updateIntervalSeconds);

    engine.update();

    engine.updateAudioBuffers(left, right, length);
    Profiler::getInstance()->stopMeasuringAudio();
}

void TrackerImpl::updateInputAudioBuffers(void* dataI2s, void* dataUsb, int length)
{
    engine.updateInputAudioBuffers(dataI2s, dataUsb, length);
}

void (*TrackerImpl::getStartDisplayFunc())(Platform*)
{
    return startDisplay;
}
