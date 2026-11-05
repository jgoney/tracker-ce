#include "AudioRuntime.h"

#include "TimerApple.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#include <unistd.h>

namespace
{
// One interrupt controller per process: the audio and the global masking pairs share the depth,
// and it stays meaningful (a plain counter) when no runtime is running.
std::atomic<int> gIrqDepth{ 0 };
std::atomic<AudioRuntime*> gActive{ nullptr };
std::atomic<pthread_t> gRenderThread{ nullptr };

// Allocation guard. -1: not resolved yet (start() resolves the default).
std::atomic<int> gGuardMode{ -1 };
std::atomic<bool> gGuardArmed{ false }; // render thread is past boot

// How long an outermost ENABLE waits for the block it unmasked (one block period, rounded up).
constexpr unsigned kPendingYieldUs = 3000;

void writeStderr(const char* msg)
{
    // write(2): no stdio lock, no allocation (render thread safe).
    ssize_t r = write(STDERR_FILENO, msg, std::strlen(msg));
    (void)r;
}

uint64_t nsToAbs(uint64_t ns)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return ns * tb.denom / tb.numer;
}

void setRenderThreadRealtime()
{
    // The render thread replaces a hardware interrupt: time-constraint scheduling, one block
    // per period.
    const uint64_t periodNs =
      (uint64_t)(1e9 * (double)AudioRuntime::kBlockFrames / AudioRuntime::kSampleRate);
    thread_time_constraint_policy_data_t policy;
    policy.period = (uint32_t)nsToAbs(periodNs);
    policy.computation = (uint32_t)nsToAbs(periodNs / 2);
    policy.constraint = (uint32_t)nsToAbs(periodNs);
    policy.preemptible = 1;
    const kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                               THREAD_TIME_CONSTRAINT_POLICY,
                                               (thread_policy_t)&policy,
                                               THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS)
        writeStderr("AudioRuntime: real-time policy refused for the render thread\n");
}
} // namespace

std::atomic<uint64_t> AudioRuntime::allocGuardViolations{ 0 };

AudioRuntime::AudioRuntime(Tracker* tracker, PlatformApple* platform, size_t ringBlocks)
  : tracker_(tracker)
  , platform_(platform)
  , ringFrames_((ringBlocks ? ringBlocks : 1) * kBlockFrames)
  , ringL_(new float[ringFrames_]())
  , ringR_(new float[ringFrames_]())
{
    const task_t task = mach_task_self();
    semaphore_create(task, &spaceSem_, SYNC_POLICY_FIFO, 0);
    semaphore_create(task, &bootSem_, SYNC_POLICY_FIFO, 0);
    semaphore_create(task, &irqEnabledSem_, SYNC_POLICY_FIFO, 0);
    semaphore_create(task, &irqServicedSem_, SYNC_POLICY_FIFO, 0);
}

AudioRuntime::~AudioRuntime()
{
    stop();
    const task_t task = mach_task_self();
    semaphore_destroy(task, spaceSem_);
    semaphore_destroy(task, bootSem_);
    semaphore_destroy(task, irqEnabledSem_);
    semaphore_destroy(task, irqServicedSem_);
    delete[] ringL_;
    delete[] ringR_;
}

void AudioRuntime::start()
{
    if (started_)
        return;
    AudioRuntime* expected = nullptr;
    if (!gActive.compare_exchange_strong(expected, this))
    {
        writeStderr("AudioRuntime: another runtime is already running\n");
        std::abort();
    }
    if (gGuardMode.load() < 0)
    {
#ifdef NDEBUG
        const char* env = std::getenv("TRACKERCE_ALLOC_GUARD");
        const bool on = env && *env && std::strcmp(env, "0") != 0;
        gGuardMode.store(on ? (int)AllocGuard::Abort : (int)AllocGuard::Off);
#else
        gGuardMode.store((int)AllocGuard::Abort);
#endif
    }

    started_ = true;
    renderRunning_ = true;
    trackerRunning_ = true;
    pendingIrq_ = false;
    trackerThread_ = std::thread(&AudioRuntime::trackerMain, this);
    // No extra port reference: valid while the pthread exists (joined after the render thread).
    trackerPort_ = pthread_mach_thread_np(trackerThread_.native_handle());
    renderThread_ = std::thread(&AudioRuntime::renderMain, this);
}

void AudioRuntime::stop()
{
    if (!started_)
        return;
    // Render thread first: it must never suspend a thread that is gone.
    renderRunning_ = false;
    semaphore_signal(spaceSem_);
    semaphore_signal(bootSem_);
    semaphore_signal(irqEnabledSem_);
    renderThread_.join();

    trackerRunning_ = false;
    trackerThread_.join();
    trackerPort_ = MACH_PORT_NULL;

    // Drop signals nobody consumed, so a restart starts clean.
    const mach_timespec_t zero{ 0, 0 };
    while (semaphore_timedwait(bootSem_, zero) == KERN_SUCCESS)
    {
    }
    while (semaphore_timedwait(irqEnabledSem_, zero) == KERN_SUCCESS)
    {
    }
    while (semaphore_timedwait(irqServicedSem_, zero) == KERN_SUCCESS)
    {
    }
    pendingIrq_ = false;
    started_ = false;
    gActive.store(nullptr);
}

// ---- tracker thread: the hardware main loop -------------------------------------------------

void AudioRuntime::trackerMain()
{
    pthread_setname_np("trackerce.tracker");
    // Above default so the real-time render thread waiting on irqEnabledSem_ is not stuck
    // behind ordinary work (priority inversion).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    // Task 4 boot order; the render thread starts rendering only after init() returns.
    tracker_->getStartDisplayFunc()(platform_);
    tracker_->init(platform_);
    semaphore_signal(bootSem_);

    while (trackerRunning_.load(std::memory_order_relaxed))
    {
        if (platform_)
            platform_->input().dispatch();
        tracker_->update();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ---- render thread: the audio interrupt -----------------------------------------------------

void AudioRuntime::renderMain()
{
    pthread_setname_np("trackerce.render");
    gRenderThread.store(pthread_self());
    setRenderThreadRealtime();
    // Instantiate thread-locals used on this thread now: first access of a thread_local
    // mallocs (dyld TLV), which would deadlock if the frozen tracker held the malloc lock.
    (void)TimerApple::currentSubBlockOffsetUs();

    semaphore_wait(bootSem_);
    if (renderRunning_.load())
    {
        gGuardArmed.store(true);
        while (renderRunning_.load(std::memory_order_relaxed))
        {
            serviceWorkgroup();
            if (!waitForSpace())
                break;

            // Masked: pend the block until the outermost ENABLE. Lost-wakeup safe: either we see
            // the depth at 0 after publishing pendingIrq_, or the enabler sees pendingIrq_ and
            // signals; if both, the exchange decides who consumes it.
            if (gIrqDepth.load() > 0)
            {
                pendingIrq_.store(true);
                if (gIrqDepth.load() > 0 || !pendingIrq_.exchange(false))
                {
                    semaphore_wait(irqEnabledSem_);
                    servicingPending_ = true;
                    continue; // re-check space, stop and depth
                }
            }
            renderOneBlock();
        }
        gGuardArmed.store(false);
    }
    signalPendingServiced();
    if (joinedWorkgroup_)
    {
        os_workgroup_leave((os_workgroup_t)joinedWorkgroup_, &joinToken_);
        joinedWorkgroup_ = nullptr;
    }
    gRenderThread.store(nullptr);
}

bool AudioRuntime::waitForSpace()
{
    while (renderRunning_.load(std::memory_order_relaxed))
    {
        const uint64_t used =
          writePos_.load(std::memory_order_relaxed) - readPos_.load(std::memory_order_acquire);
        if (used + kBlockFrames <= ringFrames_)
            return true;
        semaphore_wait(spaceSem_); // pull() signals; stop() too
    }
    return false;
}

void AudioRuntime::renderOneBlock()
{
    // Freeze the main loop: thread_suspend returns once the tracker thread is off-CPU.
    const kern_return_t kr = thread_suspend(trackerPort_);
    if (kr != KERN_SUCCESS)
    {
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            writeStderr("AudioRuntime: thread_suspend failed; rendering without freezing\n");
        }
    }
    // A DISABLE that landed between the depth check and the suspend: back off and pend.
    else if (gIrqDepth.load() > 0)
    {
        thread_resume(trackerPort_);
        return;
    }

    const uint64_t block = nextBlockIndex_;
    // ---- Task 8 (ruling R2): TimerApple::beginBlock(block) goes here ----
    tracker_->updateInputAudioBuffers(zeroI2s_, zeroUsb_, (int)kBlockFrames); // as on IMX
    tracker_->updateAudioBuffers(blockL_, blockR_, (int)kBlockFrames);
    // ---- Task 8 (ruling R2): TimerApple::endBlock() goes here ----

    if (kr == KERN_SUCCESS)
        thread_resume(trackerPort_);

    pushBlock();
    nextBlockIndex_ = block + 1;
    blocksRendered.fetch_add(1, std::memory_order_relaxed);
    if (onBlockRendered)
        onBlockRendered(block);
    signalPendingServiced();
}

void AudioRuntime::pushBlock()
{
    const uint64_t w = writePos_.load(std::memory_order_relaxed);
    // ringFrames_ is a multiple of the block size, so a block never wraps.
    const size_t at = (size_t)(w % ringFrames_);
    constexpr float kScale = 1.0f / 32768.0f;
    for (size_t i = 0; i < kBlockFrames; i++)
    {
        ringL_[at + i] = (float)blockL_[i] * kScale;
        ringR_[at + i] = (float)blockR_[i] * kScale;
    }
    writePos_.store(w + kBlockFrames, std::memory_order_release);
}

void AudioRuntime::signalPendingServiced()
{
    if (servicingPending_)
    {
        servicingPending_ = false;
        semaphore_signal(irqServicedSem_);
    }
}

void AudioRuntime::serviceWorkgroup()
{
    void* want = requestedWorkgroup_.load(std::memory_order_acquire);
    if (want == joinedWorkgroup_)
        return;
    if (joinedWorkgroup_)
        os_workgroup_leave((os_workgroup_t)joinedWorkgroup_, &joinToken_);
    joinedWorkgroup_ = nullptr;
    if (want && os_workgroup_join((os_workgroup_t)want, &joinToken_) == 0)
        joinedWorkgroup_ = want;
}

// ---- Core Audio side ------------------------------------------------------------------------

size_t AudioRuntime::pull(float* l, float* r, size_t frames)
{
    const uint64_t rd = readPos_.load(std::memory_order_relaxed);
    const uint64_t wr = writePos_.load(std::memory_order_acquire);
    const size_t avail = (size_t)(wr - rd);
    const size_t n = frames < avail ? frames : avail;

    const size_t at = (size_t)(rd % ringFrames_);
    const size_t first = n < ringFrames_ - at ? n : ringFrames_ - at;
    std::memcpy(l, ringL_ + at, first * sizeof(float));
    std::memcpy(r, ringR_ + at, first * sizeof(float));
    std::memcpy(l + first, ringL_, (n - first) * sizeof(float));
    std::memcpy(r + first, ringR_, (n - first) * sizeof(float));

    if (n > 0)
    {
        readPos_.store(rd + n, std::memory_order_release);
        semaphore_signal(spaceSem_);
    }
    if (n < frames)
    {
        std::memset(l + n, 0, (frames - n) * sizeof(float));
        std::memset(r + n, 0, (frames - n) * sizeof(float));
        if (wr > 0) // silence before the first block is start-up, not an underrun
            underruns.fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}

// ---- interrupt masking ----------------------------------------------------------------------

bool AudioRuntime::onRenderThread()
{
    const pthread_t t = gRenderThread.load(std::memory_order_relaxed);
    return t != nullptr && pthread_equal(t, pthread_self());
}

void AudioRuntime::irqDisable()
{
    if (onRenderThread())
        return; // inside the "ISR": it cannot be preempted by itself
    gIrqDepth.fetch_add(1);
}

void AudioRuntime::irqEnable()
{
    if (onRenderThread())
        return;
    int d = gIrqDepth.load();
    do
    {
        if (d <= 0)
            return; // unbalanced ENABLE: ignore, as the hardware would
    } while (!gIrqDepth.compare_exchange_weak(d, d - 1));
    if (d != 1)
        return; // still nested

    // Outermost ENABLE with a block pending: on hardware the pended IRQ is taken right here.
    // Wake the render thread and yield until that block is done (bounded, so a renderer that
    // cannot run, e.g. ring full or stopping, never hangs the caller).
    AudioRuntime* rt = gActive.load();
    if (rt && rt->pendingIrq_.exchange(false))
    {
        semaphore_signal(rt->irqEnabledSem_);
        const mach_timespec_t timeout{ 0, (clock_res_t)(kPendingYieldUs * 1000) };
        semaphore_timedwait(rt->irqServicedSem_, timeout);
    }
}

int AudioRuntime::irqDisableDepth()
{
    return gIrqDepth.load();
}

// ---- allocation guard -----------------------------------------------------------------------

void AudioRuntime::setAllocGuard(AllocGuard mode)
{
    gGuardMode.store((int)mode);
}

void AudioRuntime::allocGuardCheck(size_t size)
{
    (void)size;
    if (!gGuardArmed.load(std::memory_order_relaxed) || !onRenderThread())
        return;
    const int mode = gGuardMode.load(std::memory_order_relaxed);
    if (mode == (int)AllocGuard::Count)
        allocGuardViolations.fetch_add(1, std::memory_order_relaxed);
    else if (mode == (int)AllocGuard::Abort)
    {
        writeStderr("AudioRuntime: allocation on the render thread (audio path must not "
                    "allocate)\n");
        std::abort();
    }
}
