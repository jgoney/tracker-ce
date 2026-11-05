#pragma once
// Plays AudioRuntime's ring out through an AUHAL output unit on a selectable device.
//
//   open(id, l, r)  routes the stereo mix to output channels l and r of the device (e.g. the
//                   ES-8's 8 outputs: pick any pair); every other channel is written as silence.
//                   (l == r on a mono device or by choice: both channels are mixed to it.)
//   start()/stop()  start and stop the IOProc. The AudioRuntime keeps running either way.
//
// IOProc: real-time safe. It calls AudioRuntime::pull (+ an AudioConverter when the device
// refuses 44,100 Hz and runs at its own rate) and routes into the chosen channels. No locks, no
// allocation, no logging.
//
// BLOCKING CONTRACT. Some (virtual, third-party) devices hang inside the HAL. Every HAL call
// that can block (open, start, stop, close, listener removal) therefore runs on a dedicated
// worker thread, and the public open()/start()/stop()/destructor wait for it at most 2 s
// (open() may use up to 0.5 s of that polling a sample-rate change). A device that exceeds the
// timeout is marked unresponsive for the lifetime of the process and skipped by later opens and
// fallbacks; its wedged worker thread (and the device objects it still owns) is abandoned
// rather than waited on, so the app keeps running and can quit. The destructor never blocks
// longer than 2 s for the same reason (a wedged worker is a deliberate leak).
//
// FALLBACK. If the device dies (DeviceIsAlive, vanishes from the device list, or the IOProc
// stalls for 1 s while running), or an open fails or times out, the output moves to the system
// default output, then to the built-in output device, restarting if it was running. This runs
// on an internal serial queue (never the IO thread, never the caller); open() itself only tries
// the requested device and returns false when that fails, then the fallback follows and is
// reported through onStatus. If nothing can be opened the state is Failed and the next
// default-device or device-list change retries. The default-device change is followed only
// while we are on the default. An external sample-rate change reopens the same device at its
// new rate (through the converter) instead of fighting for the rate.
#include "AudioRuntime.h"

#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CoreAudioOutput
{
  public:
    struct DeviceInfo
    {
        AudioDeviceID id;
        std::string name;
        uint32_t outChannels;
    };

    enum class Status
    {
        Opened,   // open() succeeded on the requested device
        FellBack, // the output moved to another device (name = the new device)
        Failed    // no device could be opened (name empty); retried on the next device change
    };

    // All devices with at least one output channel.
    static std::vector<DeviceInfo> listOutputDevices();
    static AudioDeviceID defaultOutputDevice(); // kAudioObjectUnknown when there is none
    // First built-in output device (transport type built-in), kAudioObjectUnknown if none.
    static AudioDeviceID builtInOutputDevice();

    // Pure routing used by the IOProc: out is frames x channels interleaved. Everything is zero
    // except channel lc (gets l) and rc (gets r); lc == rc gets (l + r) / 2. Out-of-range
    // channels are skipped.
    static void route(const float* l,
                      const float* r,
                      size_t frames,
                      float* out,
                      uint32_t channels,
                      uint32_t lc,
                      uint32_t rc);

    explicit CoreAudioOutput(AudioRuntime& runtime);
    ~CoreAudioOutput(); // closes; waits at most 2 s

    CoreAudioOutput(const CoreAudioOutput&) = delete;
    CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

    // Opens (or re-opens) the device; the previous one is closed. Stopped afterwards. Returns
    // false if the device is unusable, unresponsive (>2 s) or has no such channel (a bad
    // channel is a caller error: no fallback). Otherwise see FALLBACK above.
    bool open(AudioDeviceID id, uint8_t leftCh = 0, uint8_t rightCh = 1);
    bool start(); // false if nothing is open or the device does not start in time
    void stop();
    bool isOpen() const;
    bool isRunning() const; // true only while a device is open and its IOProc is started
    AudioDeviceID deviceId() const;
    std::string deviceName() const;

    // (device latency + safety offset + stream latency + buffer frame size) / device rate. Does
    // not include the sample-rate converter's own latency.
    double outputLatencySeconds() const;
    double deviceSampleRate() const;
    bool usingConverter() const;
    uint32_t bufferFrames() const;

    // IOProc cycles since construction, and fallback / rate-follow reopenings.
    uint64_t cycles() const;
    uint64_t reopens() const;

    // Fires at the start of each IOProc cycle on the Core Audio IO thread (real-time: no
    // blocking). hostTimeOfFirstFrame is the mach host time at which the cycle's first output
    // frame is handed to the device (add outputLatencySeconds() for when it is heard);
    // blockIndex/offsetInBlock identify the first ring frame of this cycle (AudioRuntime's
    // 128-frame blocks). Set it while stopped.
    void setOnCycle(
      std::function<void(uint64_t hostTimeOfFirstFrame, uint64_t blockIndex, size_t offsetInBlock)>
        fn);

    // Fires on the internal control queue (never the IO thread), possibly from a different
    // thread than the caller's.
    void setOnStatus(std::function<void(Status, std::string deviceName)> fn);

    struct Impl; // opaque

  private:
    friend struct CoreAudioOutputTestAccess;
    std::shared_ptr<Impl> impl_;
};

// Test-only seams (tests/test_coreaudio.cpp, test_coreaudio_hang.cpp).
struct CoreAudioOutputTestHooks
{
    std::function<void(AudioDeviceID)> beforeOpen; // runs on the open worker first; may block forever
    std::function<bool(AudioDeviceID)> alive;      // replaces the DeviceIsAlive property read
    std::function<AudioDeviceID()> defaultDevice;  // replaces the system default lookup
    std::function<AudioDeviceID()> builtinDevice;  // replaces the built-in lookup
    std::function<void(AudioDeviceID)> beforeTeardown; // first thing in a device's close; may block forever
};

struct CoreAudioOutputTestAccess
{
    static void setHooks(CoreAudioOutput& o, CoreAudioOutputTestHooks h);
    // Same path as the HAL listener callbacks: queue hop, then the handler.
    static void postDeviceDead(CoreAudioOutput& o);
    static void postDevicesChanged(CoreAudioOutput& o);
    static void freezeWatchdogClock(CoreAudioOutput& o, bool freeze); // IOProc stops reporting liveness
    static bool isUnresponsive(CoreAudioOutput& o, AudioDeviceID id);
};
