// CoreAudioOutput against real devices (needs an audio output; built into ctest only with
// -DTRACKERCE_HW_TESTS=ON, label "hardware"). Hardware-free logic lives in test_coreaudio_logic.cpp.
#include <catch2/catch_test_macros.hpp>

#include "CoreAudioOutput.h"
#include "test_coreaudio_common.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace ca_test;
using Status = CoreAudioOutput::Status;

namespace
{
struct StatusLog
{
    std::mutex m;
    std::vector<std::pair<Status, std::string>> v;
    void attach(CoreAudioOutput& o)
    {
        o.setOnStatus([this](Status s, std::string n)
        {
            std::lock_guard<std::mutex> g(m);
            v.emplace_back(s, n);
        });
    }
    size_t count()
    {
        std::lock_guard<std::mutex> g(m);
        return v.size();
    }
    Status last()
    {
        std::lock_guard<std::mutex> g(m);
        return v.back().first;
    }
};
} // namespace

TEST_CASE("test_lists_builtin_output", "[hardware]")
{
    const auto devs = CoreAudioOutput::listOutputDevices();
    REQUIRE(!devs.empty());
    const AudioDeviceID def = CoreAudioOutput::defaultOutputDevice();
    REQUIRE(def != kAudioObjectUnknown);
    bool foundDefault = false;
    for (auto& d : devs)
    {
        REQUIRE(d.outChannels > 0);
        REQUIRE(!d.name.empty());
        foundDefault |= d.id == def;
    }
    REQUIRE(foundDefault);
    REQUIRE(CoreAudioOutput::builtInOutputDevice() != kAudioObjectUnknown);
}

TEST_CASE("test_plays_default_device", "[hardware]")
{
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    StatusLog log;
    log.attach(out);
    std::atomic<uint64_t> calls{ 0 }, lastBlock{ 0 }, badOffset{ 0 }, zeroHost{ 0 };
    out.setOnCycle([&](uint64_t host, uint64_t block, size_t off)
    {
        if (host == 0)
            zeroHost++;
        if (off >= AudioRuntime::kBlockFrames)
            badOffset++;
        lastBlock = block;
        calls++;
    });
    REQUIRE(out.open(CoreAudioOutput::defaultOutputDevice()));
    REQUIRE(out.outputLatencySeconds() > 0);
    rt.start();
    REQUIRE(out.start());
    REQUIRE(out.isRunning());
    REQUIRE(waitUntil([&] { return calls.load() > 20; }, 2000ms));
    out.stop();
    REQUIRE(!out.isRunning());
    rt.stop();
    REQUIRE(lastBlock.load() > 0);
    REQUIRE(badOffset.load() == 0);
    REQUIRE(zeroHost.load() == 0);
    REQUIRE(log.count() >= 1);
}

// Dead device whose "default" is still the dead device: falls to the built-in output. Goes through
// the listener path (queue hop + handler), with liveness injected.
TEST_CASE("test_dead_device_default_still_dead_falls_to_builtin", "[hardware]")
{
    const AudioDeviceID dev = CoreAudioOutput::builtInOutputDevice();
    REQUIRE(dev != kAudioObjectUnknown);
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    StatusLog log;
    log.attach(out);
    REQUIRE(out.open(dev));
    rt.start();
    REQUIRE(out.start());
    REQUIRE(waitUntil([&] { return out.cycles() > 10; }, 2000ms));
    const size_t statusBefore = log.count();

    std::atomic<bool> killed{ true };
    std::atomic<int> defaultCalls{ 0 }, builtinCalls{ 0 };
    CoreAudioOutputTestHooks h;
    h.alive = [&](AudioDeviceID id) { return !(killed && id == dev); };
    h.defaultDevice = [&] { defaultCalls++; return dev; }; // still the dead device
    h.builtinDevice = [&]
    {
        builtinCalls++;
        killed = false; // "replugged" by the time the built-in is tried
        return dev;
    };
    CoreAudioOutputTestAccess::setHooks(out, h);

    const uint64_t reopensBefore = out.reopens();
    CoreAudioOutputTestAccess::postDeviceDead(out);
    REQUIRE(waitUntil([&] { return out.reopens() == reopensBefore + 1; }, 5000ms));
    REQUIRE(waitUntil([&] { return log.count() > statusBefore; }, 2000ms));
    REQUIRE(log.last() == Status::FellBack);
    REQUIRE(defaultCalls.load() >= 1);
    REQUIRE(builtinCalls.load() >= 1);
    REQUIRE(out.isOpen());
    REQUIRE(out.isRunning());
    const size_t pos = rt.readPosition();
    REQUIRE(waitUntil([&] { return rt.readPosition() > pos + 4 * AudioRuntime::kBlockFrames; }, 500ms));
    out.stop();
    rt.stop();
}

// Nothing can be opened: Failed, isRunning() tells the truth, and the next device change retries.
TEST_CASE("test_failed_fallback_then_retry_on_device_change", "[hardware]")
{
    const AudioDeviceID dev = CoreAudioOutput::builtInOutputDevice();
    REQUIRE(dev != kAudioObjectUnknown);
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    StatusLog log;
    log.attach(out);
    REQUIRE(out.open(dev));
    rt.start();
    REQUIRE(out.start());

    std::atomic<bool> nothing{ true };
    CoreAudioOutputTestHooks h;
    h.alive = [&](AudioDeviceID id) { return !(nothing && id == dev); };
    h.defaultDevice = [&] { return nothing ? (AudioDeviceID)0xFFFF0002 : dev; };
    h.builtinDevice = [&] { return nothing ? (AudioDeviceID)0xFFFF0003 : dev; };
    CoreAudioOutputTestAccess::setHooks(out, h);

    const size_t n0 = log.count();
    CoreAudioOutputTestAccess::postDeviceDead(out);
    REQUIRE(waitUntil([&] { return log.count() > n0; }, 5000ms));
    REQUIRE(log.last() == Status::Failed);
    REQUIRE(!out.isOpen());
    REQUIRE(!out.isRunning());

    nothing = false; // a device appears
    const size_t n1 = log.count();
    CoreAudioOutputTestAccess::postDevicesChanged(out);
    REQUIRE(waitUntil([&] { return log.count() > n1; }, 5000ms));
    REQUIRE(log.last() == Status::FellBack);
    REQUIRE(out.isOpen());
    REQUIRE(out.isRunning()); // the intent to run survived the outage
    REQUIRE(waitUntil([&] { return out.cycles() > 0; }, 1000ms));
    out.stop();
    rt.stop();
}

// A stalled IOProc (no cycle for 1 s while running) is treated as a dead device.
TEST_CASE("test_stall_watchdog_reopens", "[hardware]")
{
    FakeTracker fake;
    AudioRuntime rt(&fake, nullptr);
    CoreAudioOutput out(rt);
    REQUIRE(out.open(CoreAudioOutput::defaultOutputDevice()));
    rt.start();
    REQUIRE(out.start());
    REQUIRE(waitUntil([&] { return out.cycles() > 10; }, 2000ms));
    CoreAudioOutputTestAccess::freezeWatchdogClock(out, true);
    REQUIRE(waitUntil([&] { return out.reopens() >= 1; }, 4000ms));
    CoreAudioOutputTestAccess::freezeWatchdogClock(out, false);
    const uint64_t c = out.cycles();
    REQUIRE(waitUntil([&] { return out.cycles() > c + 20; }, 1000ms));
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
    REQUIRE(waitUntil([&] { return out.cycles() > 10; }, 2000ms));

    setRate(48000);
    const bool followed = waitUntil([&] { return out.usingConverter(); }, 3000ms);
    if (followed)
    {
        REQUIRE(out.deviceSampleRate() == 48000.0);
        const uint64_t c0 = out.cycles();
        REQUIRE(waitUntil([&] { return out.cycles() > c0 + 20; }, 1000ms));
        REQUIRE(out.isRunning());
        REQUIRE(rt.underruns.load() == 0);
    }
    setRate(44100); // restore for the user
    out.stop();
    rt.stop();
    REQUIRE(followed);
}
