// CoreAudioOutput against real devices. Tagged [hardware]: needs an audio output, excluded from
// the default ctest run (label "hardware").
#include <catch2/catch_test_macros.hpp>

// MacTypes.h's global `Rect` clashes with display/Frame.h (see CoreAudioOutput.cpp).
#define Rect MacTypesRect
#include <CoreAudio/CoreAudio.h>
#undef Rect

#include "AudioRuntime.h"
#include "CoreAudioOutput.h"
#include "Tracker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>


namespace
{
using namespace std::chrono_literals;

void startDisplayNoop(Platform*)
{
}

class FakeTracker final : public Tracker
{
  public:
    void init(Platform*) override {}
    void update() override { std::this_thread::sleep_for(1ms); }
    void updateAudioBuffers(void* l, void* r, int length) override
    {
        // 440 Hz-ish ramp so the output is not silent
        auto* pl = (int16_t*)l;
        auto* pr = (int16_t*)r;
        for (int i = 0; i < length; i++)
        {
            phase = (phase + 1) % 100;
            pl[i] = pr[i] = (int16_t)(phase * 100);
        }
    }
    void updateInputAudioBuffers(void*, void*, int) override {}
    void (*getStartDisplayFunc())(Platform*) override { return startDisplayNoop; }
    Platform* getPlatform() override { return nullptr; }
    int phase = 0;
};

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
} // namespace

// Reaches the private fallback entry point.
struct CoreAudioOutputTestAccess
{
    static void deviceDead(CoreAudioOutput& o) { o.handleDeviceDead(); }
};

TEST_CASE("test_lists_builtin_output", "[hardware]")
{
    const auto devs = CoreAudioOutput::listOutputDevices();
    REQUIRE(!devs.empty());
    const CoreAudioOutput::DeviceId def = CoreAudioOutput::defaultOutputDevice();
    REQUIRE(def != CoreAudioOutput::kNoDevice);
    bool foundDefault = false;
    for (auto& d : devs)
    {
        REQUIRE(d.outChannels > 0);
        REQUIRE(!d.name.empty());
        foundDefault |= d.id == def;
    }
    REQUIRE(foundDefault);
}

TEST_CASE("test_plays_default_device", "[hardware]")
{
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    std::atomic<uint64_t> cycleCalls{ 0 };
    uint64_t lastBlock = 0;
    out.onCycle = [&](uint64_t host, uint64_t block, size_t off)
    {
        REQUIRE(host != 0);
        REQUIRE(off < AudioRuntime::kBlockFrames);
        lastBlock = block;
        cycleCalls++;
    };
    REQUIRE(out.open(CoreAudioOutput::defaultOutputDevice()));
    REQUIRE(out.outputLatencySeconds() > 0);
    rt.start();
    REQUIRE(out.start());
    REQUIRE(waitUntil([&] { return cycleCalls.load() > 20; }, 2000ms));
    REQUIRE(lastBlock > 0);
    out.stop();
    rt.stop();
}

TEST_CASE("test_device_loss_falls_back", "[hardware]")
{
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    REQUIRE(out.open(CoreAudioOutput::defaultOutputDevice()));
    rt.start();
    REQUIRE(out.start());
    REQUIRE(waitUntil([&] { return out.cycles.load() > 10; }, 2000ms));

    const uint64_t reopensBefore = out.reopens.load();
    const size_t posBefore = rt.readPosition();
    CoreAudioOutputTestAccess::deviceDead(out); // as if the device vanished

    REQUIRE(out.reopens.load() == reopensBefore + 1);
    REQUIRE(out.isOpen());
    REQUIRE(out.deviceId() == CoreAudioOutput::defaultOutputDevice());
    // pull() keeps being called on the new device within 500 ms
    REQUIRE(waitUntil([&] { return rt.readPosition() > posBefore + 4 * AudioRuntime::kBlockFrames; }, 500ms));
    REQUIRE(out.isRunning());
    out.stop();
    rt.stop();
}

// A rate change under us (another app, the user): follow it through the AudioConverter without
// stopping the runtime, and keep delivering audio.
TEST_CASE("test_external_rate_change_uses_converter", "[hardware]")
{
    const AudioDeviceID dev = CoreAudioOutput::defaultOutputDevice();
    AudioObjectPropertyAddress a{ kAudioDevicePropertyNominalSampleRate,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain };
    auto setRate = [&](Float64 r) { AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof r, &r); };

    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    REQUIRE(out.open(dev));
    REQUIRE(out.deviceSampleRate() == 44100.0);
    REQUIRE(!out.usingConverter());
    rt.start();
    REQUIRE(out.start());
    REQUIRE(waitUntil([&] { return out.cycles.load() > 10; }, 2000ms));

    setRate(48000);
    const bool followed = waitUntil([&] { return out.usingConverter(); }, 3000ms);
    if (followed)
    {
        REQUIRE(out.deviceSampleRate() == 48000.0);
        const uint64_t c0 = out.cycles.load();
        REQUIRE(waitUntil([&] { return out.cycles.load() > c0 + 20; }, 1000ms));
        REQUIRE(out.isRunning());
        REQUIRE(rt.underruns.load() == 0);
    }
    setRate(44100); // restore for the user
    out.stop();
    rt.stop();
    REQUIRE(followed);
}
