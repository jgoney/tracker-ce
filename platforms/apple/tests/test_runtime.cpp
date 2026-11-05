// AudioRuntime: the tracker thread is frozen while a block renders, critical sections hold the
// block off, blocking updates do not starve audio, and the render thread does not allocate.
#include <catch2/catch_test_macros.hpp>

#include "AudioRuntime.h"
#include "Tracker.h"
#include "interruptsControl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern void platformDisableGlobalInterrupts();
extern void platformEnableGlobalInterrupts();

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void spinFor(std::chrono::microseconds d)
{
    const auto end = Clock::now() + d;
    while (Clock::now() < end)
    {
    }
}

void startDisplayNoop(Platform*)
{
}

// Time-constraint scheduling like Core Audio's IO thread.
void makeRealtime()
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t periodNs = 2902494; // 128 / 44100 s
    auto abs = [&](uint64_t ns) { return (uint32_t)(ns * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t p{ abs(periodNs), abs(periodNs / 4), abs(periodNs), 1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

// Records nothing by itself: each test installs the behaviour it needs.
class FakeTracker final : public Tracker
{
  public:
    std::function<void()> onUpdate;
    std::function<void(int16_t*, int16_t*, int)> onAudio;
    std::atomic<bool> initDone{ false };
    std::atomic<uint64_t> updates{ 0 };

    void init(Platform*) override { initDone = true; }
    void update() override
    {
        if (onUpdate)
            onUpdate();
        updates++;
    }
    void updateAudioBuffers(void* l, void* r, int length) override
    {
        if (onAudio)
            onAudio((int16_t*)l, (int16_t*)r, length);
    }
    void updateInputAudioBuffers(void*, void*, int) override {}
    void (*getStartDisplayFunc())(Platform*) override { return startDisplayNoop; }
    Platform* getPlatform() override { return nullptr; }
};

// Pulls as fast as the runtime renders until pred() holds or the timeout passes.
bool pullUntil(AudioRuntime& rt,
               const std::function<bool()>& pred,
               std::chrono::milliseconds timeout)
{
    float l[AudioRuntime::kBlockFrames], r[AudioRuntime::kBlockFrames];
    const auto end = Clock::now() + timeout;
    while (!pred())
    {
        if (Clock::now() > end)
            return false;
        if (rt.pull(l, r, AudioRuntime::kBlockFrames) == 0)
            std::this_thread::yield();
    }
    return true;
}

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto end = Clock::now() + timeout;
    while (!pred())
    {
        if (Clock::now() > end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
} // namespace

TEST_CASE("test_tracker_suspended_during_block", "[runtime]")
{
    FakeTracker fake;
    std::atomic<uint64_t> counter{ 0 };
    std::atomic<bool> inUpdate{ false };
    std::atomic<uint64_t> mismatches{ 0 }, blocksInUpdate{ 0 };

    // The tracker spends almost all its time counting, so most blocks interrupt it mid-loop.
    fake.onUpdate = [&]
    {
        inUpdate = true;
        const auto end = Clock::now() + 20ms;
        while (Clock::now() < end)
            counter.fetch_add(1, std::memory_order_relaxed);
        inUpdate = false;
    };
    fake.onAudio = [&](int16_t*, int16_t*, int)
    {
        const bool during = inUpdate.load();
        const uint64_t a = counter.load();
        spinFor(100us);
        const uint64_t b = counter.load();
        if (a != b)
            mismatches++;
        if (during)
            blocksInUpdate++;
    };

    AudioRuntime rt(&fake, nullptr);
    rt.start();
    // Paced a little (~3x real time) so the tracker runs between blocks; back-to-back blocks
    // would starve it and land most blocks outside update().
    float l[AudioRuntime::kBlockFrames], r[AudioRuntime::kBlockFrames];
    const auto end = Clock::now() + 20s;
    while (rt.blocksRendered < 1000 && Clock::now() < end)
    {
        if (rt.pull(l, r, AudioRuntime::kBlockFrames) == 0)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(500us);
    }
    const bool done = rt.blocksRendered >= 1000;
    rt.stop();

    REQUIRE(done);
    REQUIRE(fake.initDone);
    REQUIRE(fake.updates > 0);
    REQUIRE(blocksInUpdate > 800); // most blocks interrupt the counting loop: the check bites
    REQUIRE(mismatches == 0);
}

TEST_CASE("test_no_block_inside_critical_section", "[runtime]")
{
    FakeTracker fake;
    constexpr int kPairs = 10000;
    std::atomic<int> pairs{ 0 };
    std::atomic<bool> inCritical{ false };
    std::atomic<uint64_t> violations{ 0 }, depthViolations{ 0 }, blocksDuringPairs{ 0 };

    // Audio, global and nested audio+global sections: they share one depth.
    fake.onUpdate = [&]
    {
        for (int i = 0; i < 200 && pairs < kPairs; i++)
        {
            const int kind = pairs % 3;
            if (kind == 0)
                DISABLE_AUDIO_INTERRUPTS();
            else if (kind == 1)
                platformDisableGlobalInterrupts();
            else
            {
                DISABLE_AUDIO_INTERRUPTS();
                platformDisableGlobalInterrupts();
            }
            inCritical = true;
            spinFor(50us);
            inCritical = false;
            if (kind == 0)
                ENABLE_AUDIO_INTERRUPTS();
            else if (kind == 1)
                platformEnableGlobalInterrupts();
            else
            {
                platformEnableGlobalInterrupts();
                ENABLE_AUDIO_INTERRUPTS();
            }
            pairs++;
        }
    };
    fake.onAudio = [&](int16_t*, int16_t*, int)
    {
        if (inCritical)
            violations++;
        if (AudioRuntime::irqDisableDepth() != 0)
            depthViolations++;
        const int p = pairs;
        if (p > 0 && p < kPairs)
            blocksDuringPairs++;
    };

    AudioRuntime rt(&fake, nullptr);
    rt.start();
    const bool done = pullUntil(rt, [&] { return pairs >= kPairs; }, 30s);
    rt.stop();

    REQUIRE(done);
    REQUIRE(blocksDuringPairs > 100); // blocks did run between the sections
    REQUIRE(violations == 0);
    REQUIRE(depthViolations == 0);
    REQUIRE(AudioRuntime::irqDisableDepth() == 0);
}

TEST_CASE("test_render_continues_during_blocking_update", "[runtime]")
{
    FakeTracker fake;
    std::atomic<bool> sleepRequested{ false }, slept{ false };
    fake.onUpdate = [&]
    {
        if (sleepRequested.exchange(false))
        {
            std::this_thread::sleep_for(200ms);
            slept = true;
        }
    };

    AudioRuntime rt(&fake, nullptr);
    rt.start();
    // Let the ring fill, as Core Audio would start pulling into a primed stream.
    REQUIRE(waitUntil([&] { return rt.blocksRendered >= 4; }, 5s));

    // Pull at the real-time rate for 1 s, the way an IOProc would: one 128-frame cycle per
    // period on a time-constraint thread (an ordinary thread oversleeps by 20 ms and then bursts,
    // which no IOProc does).
    std::atomic<uint64_t> pulled{ 0 };
    sleepRequested = true;
    std::thread ioproc(
      [&]
      {
          makeRealtime();
          float l[AudioRuntime::kBlockFrames], r[AudioRuntime::kBlockFrames];
          mach_timebase_info_data_t tb;
          mach_timebase_info(&tb);
          const double ticksPerSec = 1e9 * tb.denom / tb.numer;
          const uint64_t t0 = mach_absolute_time();
          for (uint64_t cycle = 1;; cycle++)
          {
              const uint64_t due = t0 + (uint64_t)(cycle * AudioRuntime::kBlockFrames /
                                                   AudioRuntime::kSampleRate * ticksPerSec);
              mach_wait_until(due);
              rt.pull(l, r, AudioRuntime::kBlockFrames);
              pulled += AudioRuntime::kBlockFrames;
              if (due - t0 >= (uint64_t)ticksPerSec)
                  break;
          }
      });
    ioproc.join();
    rt.stop();

    REQUIRE(slept);
    REQUIRE(pulled > 40000);
    REQUIRE(rt.underruns == 0);
}

TEST_CASE("test_pull_is_zero_filled_on_underrun", "[runtime]")
{
    FakeTracker fake;
    fake.onAudio = [](int16_t* l, int16_t* r, int n)
    {
        for (int i = 0; i < n; i++)
        {
            l[i] = 16384;
            r[i] = -16384;
        }
    };

    AudioRuntime rt(&fake, nullptr, 4);
    std::vector<float> l(600, 7.0f), r(600, 7.0f);

    // Not started: nothing delivered, all zeros, and silence before the first block is not an
    // underrun.
    REQUIRE(rt.pull(l.data(), r.data(), 64) == 0);
    for (int i = 0; i < 64; i++)
    {
        REQUIRE(l[i] == 0.0f);
        REQUIRE(r[i] == 0.0f);
    }
    REQUIRE(rt.underruns == 0);

    rt.start();
    REQUIRE(waitUntil([&] { return rt.blocksRendered >= 4; }, 5s));
    rt.stop(); // ring full (4 blocks), nothing more will be rendered

    std::fill(l.begin(), l.end(), 7.0f);
    std::fill(r.begin(), r.end(), 7.0f);
    REQUIRE(rt.pull(l.data(), r.data(), 600) == 512);
    for (int i = 0; i < 512; i++)
    {
        REQUIRE(l[i] == 0.5f);
        REQUIRE(r[i] == -0.5f);
    }
    for (int i = 512; i < 600; i++)
    {
        REQUIRE(l[i] == 0.0f);
        REQUIRE(r[i] == 0.0f);
    }
    REQUIRE(rt.underruns == 1);
    REQUIRE(rt.readPosition() == 512);
    REQUIRE(rt.blockIndexAt(rt.readPosition()) == 4);
    REQUIRE(rt.blockIndexAt(127) == 0);
}

TEST_CASE("test_global_irq_disable_from_render_thread_is_noop", "[runtime]")
{
    FakeTracker fake;
    std::atomic<int> maxDepthInBlock{ 0 };
    fake.onAudio = [&](int16_t*, int16_t*, int)
    {
        for (int i = 0; i < 100; i++)
        {
            platformDisableGlobalInterrupts();
            DISABLE_AUDIO_INTERRUPTS(); // AudioNoInterrupts() from play_memory.cpp, same path
            const int d = AudioRuntime::irqDisableDepth();
            if (d > maxDepthInBlock)
                maxDepthInBlock = d;
            ENABLE_AUDIO_INTERRUPTS();
            platformEnableGlobalInterrupts();
        }
    };

    AudioRuntime rt(&fake, nullptr);
    rt.start();
    const auto t0 = Clock::now();
    pullUntil(rt, [&] { return Clock::now() - t0 >= 1s; }, 2s);
    const uint64_t blocks = rt.blocksRendered;
    rt.stop();

    REQUIRE(blocks > 300); // no deadlock: rendering kept going for the whole second
    REQUIRE(maxDepthInBlock == 0);
    REQUIRE(AudioRuntime::irqDisableDepth() == 0);
}

TEST_CASE("test_render_thread_malloc_aborts_in_debug", "[runtime]")
{
    SECTION("violations are detected on the render thread only (Count mode)")
    {
        AudioRuntime::setAllocGuard(AudioRuntime::AllocGuard::Count);
        const uint64_t before = AudioRuntime::allocGuardViolations;

        FakeTracker fake;
        std::atomic<uint64_t> block{ 0 };
        std::vector<int>* trackerSide = nullptr;
        fake.onUpdate = [&]
        {
            delete trackerSide;
            trackerSide = new std::vector<int>(16); // tracker thread: allowed
        };
        fake.onAudio = [&](int16_t*, int16_t*, int)
        {
            if (block++ == 10)
            {
                int* volatile p = new int(5); // render thread: a violation
                delete p;
            }
        };

        AudioRuntime rt(&fake, nullptr);
        rt.start();
        const bool done = pullUntil(rt, [&] { return rt.blocksRendered >= 50; }, 5s);
        rt.stop();
        delete trackerSide;
        AudioRuntime::setAllocGuard(AudioRuntime::AllocGuard::Off);

        REQUIRE(done);
        REQUIRE(fake.updates > 0);
        REQUIRE(AudioRuntime::allocGuardViolations - before == 1);
    }

    SECTION("Abort mode kills the process (death test, forked child)")
    {
        const pid_t pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0)
        {
            // No crash report for the expected abort.
            task_set_exception_ports(mach_task_self(),
                                     EXC_MASK_CRASH | EXC_MASK_CORPSE_NOTIFY,
                                     MACH_PORT_NULL,
                                     EXCEPTION_DEFAULT,
                                     THREAD_STATE_NONE);
            AudioRuntime::setAllocGuard(AudioRuntime::AllocGuard::Abort);
            FakeTracker fake;
            fake.onAudio = [](int16_t*, int16_t*, int)
            {
                int* volatile p = new int(5);
                delete p;
            };
            AudioRuntime rt(&fake, nullptr);
            rt.start();
            float l[128], r[128];
            const auto end = Clock::now() + 5s;
            while (Clock::now() < end)
                rt.pull(l, r, 128);
            _exit(0); // survived: the guard did not fire
        }
        int status = 0;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFSIGNALED(status));
        REQUIRE(WTERMSIG(status) == SIGABRT);
    }
}
