// trackerce-headless: boots the Tracker core on the Apple HAL without a window.
//
//   trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] [--sd DIR]
//                      [--audio-seconds S]
//
// Runs N core update() calls (default 500), M milliseconds apart in wall-clock time (default 10:
// the UI is time driven, e.g. the start screen shows for at least ~2 s), then rasterises the
// last frame the FT812 model published with CpuRaster and writes it as a binary PPM.
// With --audio-seconds, AudioRuntime drives the core instead (tracker + render threads) for S
// seconds, pulled by a simulated real-time IOProc (no device), with Play pressed after 3 s; it
// prints blocks rendered and underruns. TRACKERCE_ALLOC_GUARD=1 aborts on any allocation in
// the audio path.
// The SD card root defaults to a fresh temporary folder.
#include "AudioRuntime.h"
#include "CpuRaster.h"
#include "PlatformApple.h"
#include "Tracker.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace
{
int usage()
{
    std::fprintf(stderr,
                 "usage: trackerce-headless --screenshot out.ppm [--updates N] [--interval-ms M] "
                 "[--sd DIR] [--audio-seconds S]\n");
    return 2;
}

// Stands in for Core Audio's IO thread: time-constraint policy, one 128-frame pull per period.
void simulatedIoProc(AudioRuntime& rt, double seconds, float& peak)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double ticksPerSec = 1e9 * tb.denom / tb.numer;
    const double period = AudioRuntime::kBlockFrames / AudioRuntime::kSampleRate;
    thread_time_constraint_policy_data_t pol{ (uint32_t)(period * ticksPerSec),
                                              (uint32_t)(period * ticksPerSec / 4),
                                              (uint32_t)(period * ticksPerSec),
                                              1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&pol,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    float l[AudioRuntime::kBlockFrames], r[AudioRuntime::kBlockFrames];
    const uint64_t t0 = mach_absolute_time();
    for (uint64_t cycle = 1; cycle * period < seconds; cycle++)
    {
        mach_wait_until(t0 + (uint64_t)(cycle * period * ticksPerSec));
        const size_t n = rt.pull(l, r, AudioRuntime::kBlockFrames);
        for (size_t i = 0; i < n; i++)
            peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
    }
}
} // namespace

int main(int argc, char** argv)
{
    const char* screenshot = nullptr;
    long updates = 500;
    long intervalMs = 10;
    std::string sdRoot;
    double audioSeconds = 0;

    for (int i = 1; i < argc; i++)
    {
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(argv[i], "--screenshot") && hasValue)
            screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--updates") && hasValue)
            updates = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--interval-ms") && hasValue)
            intervalMs = std::strtol(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--sd") && hasValue)
            sdRoot = argv[++i];
        else if (!std::strcmp(argv[i], "--audio-seconds") && hasValue)
            audioSeconds = std::strtod(argv[++i], nullptr);
        else
            return usage();
    }
    if (!screenshot || updates < 0 || intervalMs < 0)
        return usage();

    if (sdRoot.empty())
    {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp/") + "/trackerce_sd_XXXXXX";
        if (!mkdtemp(tmpl.data()))
        {
            std::perror("mkdtemp");
            return 1;
        }
        sdRoot = tmpl;
    }

    static PlatformApple platform(sdRoot);
    Tracker* tracker = Tracker::getInstance();
    if (audioSeconds > 0)
    {
        AudioRuntime rt(tracker, &platform);
        rt.start(); // boots the core on the tracker thread
        float peak = 0;
        std::thread io([&] { simulatedIoProc(rt, audioSeconds, peak); });
        if (audioSeconds > 3.5)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            platform.input().post({ InputEvent::Button, 23, true, 0 }); // interfaceButtonPlay
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            platform.input().post({ InputEvent::Button, 23, false, 0 });
        }
        io.join();
        rt.stop();
        std::printf("audio: %.1f s, %llu blocks rendered, %llu underruns, peak %.4f\n",
                    audioSeconds,
                    (unsigned long long)rt.blocksRendered.load(),
                    (unsigned long long)rt.underruns.load(),
                    peak);
    }
    else
    {
        tracker->getStartDisplayFunc()(&platform);
        tracker->init(&platform);

        for (long i = 0; i < updates; i++)
        {
            tracker->update();
            if (intervalMs > 0)
                usleep(useconds_t(intervalMs * 1000));
        }
    }

    auto frame = platform.display().latestFrame();
    if (!frame)
    {
        std::fprintf(stderr, "no frame was published\n");
        return 1;
    }
    if (!CpuRaster::toPPM(*frame, platform.display(), screenshot))
    {
        std::fprintf(stderr, "cannot write %s\n", screenshot);
        return 1;
    }
    std::printf("frame %llu: %zu prims -> %s\n",
                (unsigned long long)frame->serial,
                frame->prims.size(),
                screenshot);
    return 0;
}
