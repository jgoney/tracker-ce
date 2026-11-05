#pragma once
// Plays AudioRuntime's ring out through an AUHAL output unit on a selectable device.
//
//   open(id, l, r)  routes the stereo mix to output channels l and r of the device (e.g. the
//                   ES-8's 8 outputs: pick any pair); every other channel is written as silence.
//   start()/stop()  start and stop the IOProc. The AudioRuntime keeps running either way.
//
// The render callback is real-time safe: it calls AudioRuntime::pull (and an AudioConverter in
// the sample-rate fallback path) and deinterleaves into the chosen channels. No locks, no
// allocation. If the device refuses 44,100 Hz it is opened at its own rate and an AudioConverter
// resamples (latency of the converter is not included in outputLatencySeconds()).
//
// Device loss (kAudioDevicePropertyDeviceIsAlive), a change of the default output device (only
// followed once we are on the default) and an external sample-rate change are handled on an
// internal serial queue: the unit is rebuilt on the new device, restarted if it was running,
// and the runtime's render thread is pointed at the new device's IO workgroup.
#include "AudioRuntime.h"

// No Core Audio headers here on purpose: MacTypes.h declares a global `Rect` that clashes with
// display/Frame.h (pulled in by AudioRuntime.h), so only CoreAudioOutput.cpp sees them.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class CoreAudioOutput
{
  public:
    using DeviceId = uint32_t; // AudioDeviceID
    static constexpr DeviceId kNoDevice = 0; // kAudioObjectUnknown

    struct DeviceInfo
    {
        DeviceId id;
        std::string name;
        uint32_t outChannels;
    };

    // All devices with at least one output channel.
    static std::vector<DeviceInfo> listOutputDevices();
    static DeviceId defaultOutputDevice(); // kNoDevice when there is none

    explicit CoreAudioOutput(AudioRuntime& runtime);
    ~CoreAudioOutput(); // stop()s and closes

    CoreAudioOutput(const CoreAudioOutput&) = delete;
    CoreAudioOutput& operator=(const CoreAudioOutput&) = delete;

    // Opens (or re-opens) the device; the previous one is closed. Stopped afterwards. Fails when
    // the device has no such channel (rightCh may exceed the channel count of a mono device: it
    // then duplicates leftCh).
    bool open(DeviceId id, uint8_t leftCh = 0, uint8_t rightCh = 1);
    bool start();
    void stop();
    bool isOpen() const;
    bool isRunning() const { return running_.load(); }
    DeviceId deviceId() const { return deviceId_.load(); }

    // (device latency + safety offset + stream latency + buffer frame size) / device rate.
    double outputLatencySeconds() const { return latencySeconds_.load(); }
    double deviceSampleRate() const { return deviceRate_.load(); }
    bool usingConverter() const { return usingConverter_.load(); }
    uint32_t bufferFrames() const { return bufferFrames_.load(); }

    // Count of IOProc cycles and device re-openings (fallbacks), for tests and diagnostics.
    std::atomic<uint64_t> cycles{ 0 };
    std::atomic<uint64_t> reopens{ 0 };

    // Fires at the start of each IOProc cycle, on the Core Audio IO thread (real-time: no
    // blocking). hostTimeOfFirstFrame is the mach host time at which the cycle's first output
    // frame is handed to the device (add outputLatencySeconds() for when it is heard);
    // blockIndex/offsetInBlock identify the first ring frame of this cycle (AudioRuntime's
    // 128-frame blocks). Set it while stopped.
    std::function<void(uint64_t hostTimeOfFirstFrame, uint64_t blockIndex, size_t offsetInBlock)>
      onCycle;

  private:
    friend struct CoreAudioOutputTestAccess;

    struct Impl; // Core Audio callbacks and helpers (CoreAudioOutput.cpp)
    friend struct Impl;

    bool openLocked(DeviceId id, uint8_t l, uint8_t r, bool requestRate);
    void closeLocked();
    void reopenDefault(DeviceId avoid);
    void handleDeviceDead();     // device lost: reopen the default output, restart if running
    void handleDefaultChanged(); // follow the new default when we are on the default
    void handleRateChanged();    // sample rate changed under us: reopen the same device
    void addListeners();
    void removeListeners();
    void joinWorkgroup(DeviceId id);

    AudioRuntime& rt_;
    std::mutex mutex_; // control paths only, never the IOProc
    void* queue_ = nullptr; // dispatch_queue_t, serial

    void* unit_ = nullptr;      // AudioUnit (AUHAL)
    void* converter_ = nullptr; // AudioConverterRef
    std::atomic<DeviceId> deviceId_{ kNoDevice };
    uint8_t leftCh_ = 0, rightCh_ = 1;
    uint32_t channels_ = 0;
    uint32_t maxFrames_ = 0;
    bool followingDefault_ = false;
    bool listening_ = false;
    DeviceId listenedDevice_ = kNoDevice;
    std::atomic<bool> running_{ false };
    std::atomic<double> deviceRate_{ AudioRuntime::kSampleRate };
    std::atomic<double> latencySeconds_{ 0 };
    std::atomic<bool> usingConverter_{ false };
    std::atomic<uint32_t> bufferFrames_{ 0 };

    // IOProc scratch (allocated in open, used only by the IOProc while running).
    std::vector<float> srcL_, srcR_; // runtime-rate frames
    std::vector<float> mixOut_;      // converter output, stereo interleaved at the device rate
    size_t srcCap_ = 0;

    // Device workgroups are deliberately never released: the render thread may still be joined
    // to the previous one until it next renders, and a few tiny objects per device switch are
    // cheaper than a use-after-free.
    void* workgroup_ = nullptr;
};
