#pragma once
// Interrupt-faithful runtime: reproduces the hardware's "main loop + audio DMA interrupt" on two
// host threads so the unmodified core sees ISR semantics.
//
//   tracker thread  plays the main loop: startDisplay, init(), then dispatch() + update() forever.
//   render thread   plays the audio interrupt: per 128-frame block it thread_suspend()s the
//                   tracker thread, runs updateInputAudioBuffers + updateAudioBuffers, resumes
//                   it, and pushes the block into an SPSC ring that pull() (Core Audio) drains.
//
// DISABLE/ENABLE_AUDIO_INTERRUPTS and DISABLE/ENABLE_GLOBAL_INTERRUPTS (hal/interrupts_apple.cpp)
// share one nesting depth. While it is > 0 no block starts; the block pends and is taken as soon
// as the outermost ENABLE drops the depth to 0 (the enabling thread yields to it, as a pended
// IRQ fires on CPSIE). Both are no-ops on the render thread (an ISR cannot preempt itself).
//
// Code reached from updateAudioBuffers runs while the tracker thread is frozen at an arbitrary
// instruction, so it must not take any lock the tracker thread could hold, and must not allocate
// (malloc has locks). The allocation guard (AllocGuardNew.cpp) enforces the latter.
#include "PlatformApple.h"
#include "Tracker.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mach/mach.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <thread>

class AudioRuntime
{
  public:
    static constexpr size_t kBlockFrames = 128; // kAudioBlockSamples
    static constexpr double kSampleRate = 44100.0;

    // platform may be null (tests): input dispatch is then skipped and nothing is passed to the
    // tracker's startDisplay / init.
    AudioRuntime(Tracker* tracker, PlatformApple* platform, size_t ringBlocks = 4);
    ~AudioRuntime(); // stop()s

    AudioRuntime(const AudioRuntime&) = delete;
    AudioRuntime& operator=(const AudioRuntime&) = delete;

    // Spawns the tracker thread (startDisplay, init, then the update loop) and the render thread,
    // which renders nothing until init() has returned. One runtime may run at a time.
    void start();
    // Ends both threads. Blocks until the current update() returns. The ring keeps its contents.
    void stop();

    // Real-time safe (no locks, no allocation; Core Audio IOProc). Copies up to `frames` frames
    // from the ring and returns how many were delivered; the rest of l/r is zero-filled. A short
    // delivery after the first block was rendered counts one underrun.
    size_t pull(float* l, float* r, size_t frames);

    // Ring positions are monotonic frame counters: frame f of the output stream is frame
    // f % 128 of block f / 128. readPosition() is the position of the next frame pull() returns.
    uint64_t blockIndexAt(size_t ringReadPos) const { return ringReadPos / kBlockFrames; }
    size_t readPosition() const { return (size_t)readPos_.load(std::memory_order_acquire); }

    std::atomic<uint64_t> underruns{ 0 };
    std::atomic<uint64_t> blocksRendered{ 0 };

    // Render thread, after each block is in the ring (Task 8). Must be real-time safe. Set it
    // before start().
    std::function<void(uint64_t blockIndex)> onBlockRendered;

    // Join the audio device's os_workgroup (Task 7, when available); null leaves it. Takes effect
    // on the render thread before its next block. wg is an os_workgroup_t (void* so this header
    // compiles the same in C++ and ARC Objective-C++: pass (__bridge void*)wg); the caller keeps
    // it alive until it is replaced or the runtime stops.
    void setWorkgroup(void* wg) { requestedWorkgroup_.store(wg, std::memory_order_release); }

    // The core's interrupt masking (normally reached via interrupts_apple.cpp). Nesting-counted,
    // shared between the audio and the global pair, no-ops on the render thread.
    void disableAudioInterrupt() { irqDisable(); }
    void enableAudioInterrupt() { irqEnable(); }
    static void irqDisable();
    static void irqEnable();
    static int irqDisableDepth();
    // True on the render thread of the running runtime (inside the "ISR").
    static bool onRenderThread();

    // Allocation guard: what operator new does when called on the render thread after init().
    // Default: Abort when built without NDEBUG or with TRACKERCE_ALLOC_GUARD=1 in the
    // environment, else Off. Only binaries that link the TrackerAllocGuard object library have
    // the operator new hook.
    enum class AllocGuard
    {
        Off,
        Abort, // message to stderr, then abort()
        Count  // count in allocGuardViolations (tests)
    };
    static void setAllocGuard(AllocGuard mode);
    static std::atomic<uint64_t> allocGuardViolations;
    // Called by the operator new hook on every allocation.
    static void allocGuardCheck(size_t size);

  private:
    void trackerMain();
    void renderMain();
    bool waitForSpace();   // false when stopping
    void renderOneBlock(); // tracker thread suspended around the core calls
    void pushBlock();
    void serviceWorkgroup();
    void signalPendingServiced();

    Tracker* tracker_;
    PlatformApple* platform_;
    const size_t ringFrames_;
    float* ringL_;
    float* ringR_;
    std::atomic<uint64_t> writePos_{ 0 };
    std::atomic<uint64_t> readPos_{ 0 };

    int16_t blockL_[kBlockFrames]{};
    int16_t blockR_[kBlockFrames]{};
    // Silent recorder input (I2S and USB), int16 stereo interleaved as on IMX; the core keeps
    // the pointers, so they live as long as the runtime.
    uint32_t zeroI2s_[kBlockFrames]{};
    uint32_t zeroUsb_[kBlockFrames]{};
    uint64_t nextBlockIndex_ = 0;

    std::thread trackerThread_;
    std::thread renderThread_;
    mach_port_t trackerPort_ = MACH_PORT_NULL;
    std::atomic<bool> renderRunning_{ false };
    std::atomic<bool> trackerRunning_{ false };
    bool started_ = false;

    // Mach semaphores: semaphore_signal is real-time safe (pull() signals spaceSem_).
    semaphore_t spaceSem_ = MACH_PORT_NULL;       // pull() freed ring space
    semaphore_t bootSem_ = MACH_PORT_NULL;        // tracker init() returned
    semaphore_t irqEnabledSem_ = MACH_PORT_NULL;  // depth dropped to 0 with a block pending
    semaphore_t irqServicedSem_ = MACH_PORT_NULL; // that pending block was rendered

    std::atomic<bool> pendingIrq_{ false };
    bool servicingPending_ = false; // render thread only

    std::atomic<void*> requestedWorkgroup_{ nullptr };
    void* joinedWorkgroup_ = nullptr;
    os_workgroup_join_token_s joinToken_{};
};
